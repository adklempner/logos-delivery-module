#include "delivery_module_plugin.h"
#include "logos_sdk.h"
#include <cstdio>
#include <ctime>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <semaphore>
#include <unordered_map>

#include <nlohmann/json.hpp>
#include <boost/beast/core/detail/base64.hpp>

#include <QByteArray>
#include <QDebug>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRandomGenerator>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QVariant>
#include <QVariantList>

#include "api_call_handler.h"
#include "liblogos_rln_module_api.h"
#include <logos_api_consumer.h>
#include <logos_object.h>
extern "C" {
#include <liblogosdelivery.h>
}

namespace {
namespace b64 = boost::beast::detail::base64;

std::string base64Encode(const std::vector<uint8_t>& data) {
    std::string out;
    out.resize(b64::encoded_size(data.size()));
    out.resize(b64::encode(out.data(), data.data(), data.size()));
    return out;
}

std::vector<uint8_t> base64Decode(const std::string& encoded) {
    std::vector<uint8_t> out;
    out.resize(b64::decoded_size(encoded.size()));
    auto [written, read] = b64::decode(out.data(), encoded.data(), encoded.size());
    out.resize(written);
    return out;
}

int64_t currentTimestampNs() {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000000000LL + static_cast<int64_t>(ts.tv_nsec);
}
} // namespace

DeliveryModuleImpl::DeliveryModuleImpl() : deliveryCtx(nullptr)
{
    fprintf(stderr, "DeliveryModuleImpl: Initializing...\n");
    fprintf(stderr, "DeliveryModuleImpl: Initialized successfully\n");
}

DeliveryModuleImpl::~DeliveryModuleImpl()
{
    if (deliveryCtx) {
        logosdelivery_destroy(deliveryCtx, nullptr, nullptr);
        deliveryCtx = nullptr;
    }
}

// onContextReady is intentionally NOT overridden — we use lazy access via
// modules().api at call sites (gated on isContextReady()) instead, because
// calling modules() inside the override segfaulted (the codegen-emitted
// provider's onInit doesn't reliably set m_logosModulesPtr BEFORE
// _logosCoreSetContext_ fires onContextReady for empty-deps modules).

void DeliveryModuleImpl::event_callback(int callerRet, const char* msg, size_t len, void* userData)
{
    fprintf(stderr, "DeliveryModuleImpl::event_callback called with ret: %d\n", callerRet);

    DeliveryModuleImpl* impl = static_cast<DeliveryModuleImpl*>(userData);
    if (!impl) {
        fprintf(stderr, "DeliveryModuleImpl::event_callback: Invalid userData\n");
        return;
    }

    if (msg && len > 0) {
        std::string message(msg, len);
        fprintf(stderr, "DeliveryModuleImpl::event_callback message: %s\n", message.c_str());

        nlohmann::json jsonObj;
        try {
            jsonObj = nlohmann::json::parse(message);
        } catch (const nlohmann::json::parse_error&) {
            fprintf(stderr, "DeliveryModuleImpl::event_callback: Invalid JSON\n");
            return;
        }

        if (!jsonObj.is_object()) {
            fprintf(stderr, "DeliveryModuleImpl::event_callback: Invalid JSON\n");
            return;
        }

        std::string eventType = jsonObj.value("eventType", "");
        int64_t timestamp = currentTimestampNs();

        if (eventType == "message_sent") {
            impl->messageSent(
                jsonObj.value("requestId", ""),
                jsonObj.value("messageHash", ""),
                timestamp);

        } else if (eventType == "message_error") {
            impl->messageError(
                jsonObj.value("requestId", ""),
                jsonObj.value("messageHash", ""),
                jsonObj.value("error", ""),
                timestamp);

        } else if (eventType == "message_propagated") {
            impl->messagePropagated(
                jsonObj.value("requestId", ""),
                jsonObj.value("messageHash", ""),
                timestamp);

        } else if (eventType == "message_received") {
            auto msgObj = jsonObj.value("message", nlohmann::json::object());

            std::string hash = jsonObj.value("messageHash", "");
            std::string topic = msgObj.value("contentTopic", "");

            std::vector<uint8_t> payloadBytes;
            if (msgObj.contains("payload")) {
                auto& payloadValue = msgObj["payload"];
                if (payloadValue.is_array()) {
                    payloadBytes.reserve(payloadValue.size());
                    for (const auto& val : payloadValue) {
                        payloadBytes.push_back(static_cast<uint8_t>(val.get<int>()));
                    }
                } else if (payloadValue.is_string()) {
                    payloadBytes = base64Decode(payloadValue.get<std::string>());
                }
            }

            int64_t msgTimestamp = static_cast<int64_t>(msgObj.value("timestamp", 0.0));
            impl->messageReceived(hash, topic, payloadBytes, msgTimestamp);

        } else if (eventType == "connection_status_change") {
            impl->connectionStateChanged(
                jsonObj.value("connectionStatus", ""),
                timestamp);

        } else {
            fprintf(stderr, "DeliveryModuleImpl::event_callback: Unknown event type: %s\n", eventType.c_str());
        }
    }
}

// Default every listening port (tcpPort, discv5UdpPort, restPort,
// metricsServerPort, websocketPort) to 0 so the OS assigns an ephemeral port
// when the caller did not pin a specific value. Caller-supplied ports are
// preserved so fleet configs that pin ports keep working. logos-delivery now
// accepts port 0 (status-im/nim-confutils#146), which makes this work.
// See logos-delivery-module#18.
static std::optional<std::string> applyPortDefaults(const std::string& cfg)
{
    nlohmann::json cfgObj;
    try {
        cfgObj = nlohmann::json::parse(cfg);
    } catch (const nlohmann::json::parse_error&) {
        fprintf(stderr, "DeliveryModuleImpl: createNode cfg is not valid JSON\n");
        return std::nullopt;
    }

    if (!cfgObj.is_object()) {
        fprintf(stderr, "DeliveryModuleImpl: createNode cfg is not a JSON object\n");
        return std::nullopt;
    }

    for (const char* portKey : {
             "tcpPort",
             "discv5UdpPort",
             "restPort",
             "metricsServerPort",
             "websocketPort",
         }) {
        if (!cfgObj.contains(portKey)) {
            cfgObj[portKey] = 0;
        }
    }

    return cfgObj.dump();
}

StdLogosResult DeliveryModuleImpl::createNode(const std::string& cfg)
{
    std::lock_guard<std::mutex> createNodeLock(createNodeMutex);

    if (deliveryCtx != nullptr) {
        fprintf(stderr, "DeliveryModuleImpl: createNode rejected - context already initialized\n");
        return {false, {}, "Context already initialized"};
    }

    // Don't log cfg: it can carry sensitive config.
    fprintf(stderr, "DeliveryModuleImpl::createNode called\n");

    auto cfgWithDefaults = applyPortDefaults(cfg);
    if (!cfgWithDefaults) {
        return {false, {}, "Invalid JSON config"};
    }
    const std::string& cfgWithPorts = *cfgWithDefaults;

    struct CallbackContext {
        std::binary_semaphore sem{0};
        int callerRet{RET_ERR};
        std::string message;
    };

    static std::mutex pendingMutex;
    static std::unordered_map<void*, std::shared_ptr<CallbackContext>> pendingContexts;

    auto callbackCtx = std::make_shared<CallbackContext>();
    void* callbackKey = static_cast<void*>(callbackCtx.get());

    {
        std::lock_guard<std::mutex> lock(pendingMutex);
        pendingContexts[callbackKey] = callbackCtx;
    }

    auto callback = +[](int callerRet, const char* msg, size_t len, void* userData) {
        fprintf(stderr, "DeliveryModuleImpl::createNode callback called with ret: %d\n", callerRet);

        std::shared_ptr<CallbackContext> callbackCtx;
        {
            std::lock_guard<std::mutex> lock(pendingMutex);
            auto it = pendingContexts.find(userData);
            if (it == pendingContexts.end()) {
                return;
            }
            callbackCtx = it->second;
            pendingContexts.erase(it);
        }

        if (!callbackCtx) {
            return;
        }

        callbackCtx->callerRet = callerRet;
        if (msg && len > 0) {
            callbackCtx->message = std::string(msg, len);
            fprintf(stderr, "DeliveryModuleImpl::createNode callback message: %s\n", callbackCtx->message.c_str());
        }

        callbackCtx->sem.release();
    };

    deliveryCtx = logosdelivery_create_node(cfgWithPorts.c_str(), callback, callbackKey);

    fprintf(stderr, "DeliveryModuleImpl: Waiting for createNode callback...\n");

    if (!callbackCtx->sem.try_acquire_for(CALLBACK_TIMEOUT)) {
        std::lock_guard<std::mutex> lock(pendingMutex);
        pendingContexts.erase(callbackKey);

        deliveryCtx = nullptr;

        fprintf(stderr, "DeliveryModuleImpl: Timeout waiting for createNode callback\n");
        return {false, {}, "Timeout waiting for createNode callback"};
    }

    if (callbackCtx->callerRet != RET_OK || deliveryCtx == nullptr) {
        if (!callbackCtx->message.empty()) {
            fprintf(stderr, "DeliveryModuleImpl: createNode callback error: %s\n", callbackCtx->message.c_str());
        }

        deliveryCtx = nullptr;

        fprintf(stderr, "DeliveryModuleImpl: Failed to create Delivery context\n");
        return {false, {}, "Failed to create Delivery context"};
    }

    fprintf(stderr, "DeliveryModuleImpl: Delivery context created successfully\n");

    logosdelivery_set_event_callback(deliveryCtx, event_callback, this);
    return {true, {}};
}

StdLogosResult DeliveryModuleImpl::start()
{
    fprintf(stderr, "DeliveryModuleImpl::start called\n");

    if (!deliveryCtx) {
        fprintf(stderr, "DeliveryModuleImpl: Cannot start Delivery - context not initialized. Call createNode first.\n");
        return {false, {}, "Context not initialized"};
    }

    auto outcome = callApiRetVoid(
        "start",
        CALLBACK_TIMEOUT,
        bindApiCall(logosdelivery_start_node, deliveryCtx));

    if (!outcome.success) {
        fprintf(stderr, "DeliveryModuleImpl: Start failed: %s\n", outcome.error.c_str());
    }

    fprintf(stderr, "DeliveryModuleImpl: Delivery start completed with success\n");
    return outcome;
}

StdLogosResult DeliveryModuleImpl::stop()
{
    fprintf(stderr, "DeliveryModuleImpl::stop called\n");

    if (!deliveryCtx) {
        fprintf(stderr, "DeliveryModuleImpl: Cannot stop Delivery - context not initialized. Call createNode first.\n");
        return {false, {}, "Context not initialized"};
    }

    auto outcome = callApiRetVoid(
        "stop",
        CALLBACK_TIMEOUT,
        bindApiCall(logosdelivery_stop_node, deliveryCtx));

    if (!outcome.success) {
        fprintf(stderr, "DeliveryModuleImpl: Stop failed: %s\n", outcome.error.c_str());
    }

    fprintf(stderr, "DeliveryModuleImpl: Delivery stop completed with success\n");
    return outcome;
}

StdLogosResult DeliveryModuleImpl::send(const std::string& contentTopic, const std::vector<uint8_t>& payload)
{
    fprintf(stderr, "DeliveryModuleImpl::send called with contentTopic: %s\n", contentTopic.c_str());

    if (!deliveryCtx) {
        fprintf(stderr, "DeliveryModuleImpl: Cannot send message - context not initialized. Call createNode first.\n");
        return {false, {}, "Context not initialized"};
    }

    nlohmann::json messageObj;
    messageObj["contentTopic"] = contentTopic;
    messageObj["payload"] = base64Encode(payload);
    messageObj["ephemeral"] = false;

    std::string messageJson = messageObj.dump();

    auto outcome = callApiRetValue(
        "send",
        CALLBACK_TIMEOUT,
        bindApiCall(logosdelivery_send, deliveryCtx, messageJson.c_str()));

    if (!outcome.success) {
        fprintf(stderr, "DeliveryModuleImpl: Send failed for topic: %s, reason: %s\n",
                contentTopic.c_str(), outcome.error.c_str());
    }

    if (outcome.success && outcome.value.is_string()) {
        fprintf(stderr, "DeliveryModuleImpl: Send initiated for topic: %s, with success, requestId: %s\n",
                contentTopic.c_str(), outcome.value.get<std::string>().c_str());
    }
    return outcome;
}

StdLogosResult DeliveryModuleImpl::subscribe(const std::string& contentTopic)
{
    fprintf(stderr, "DeliveryModuleImpl::subscribe called with contentTopic: %s\n", contentTopic.c_str());

    if (!deliveryCtx) {
        fprintf(stderr, "DeliveryModuleImpl: Cannot subscribe - context not initialized. Call createNode first.\n");
        return {false, {}, "Context not initialized"};
    }

    auto outcome = callApiRetVoid(
        "subscribe",
        CALLBACK_TIMEOUT,
        bindApiCall(logosdelivery_subscribe, deliveryCtx, contentTopic.c_str()));

    if (!outcome.success) {
        fprintf(stderr, "DeliveryModuleImpl: Subscribe failed for topic: %s, reason: %s\n",
                contentTopic.c_str(), outcome.error.c_str());
    }

    fprintf(stderr, "DeliveryModuleImpl: Subscribe completed for topic: %s with success\n", contentTopic.c_str());
    return outcome;
}

StdLogosResult DeliveryModuleImpl::unsubscribe(const std::string& contentTopic)
{
    fprintf(stderr, "DeliveryModuleImpl::unsubscribe called with contentTopic: %s\n", contentTopic.c_str());

    if (!deliveryCtx) {
        fprintf(stderr, "DeliveryModuleImpl: Cannot unsubscribe - context not initialized.\n");
        return {false, {}, "Context not initialized"};
    }

    auto outcome = callApiRetVoid(
        "unsubscribe",
        CALLBACK_TIMEOUT,
        bindApiCall(logosdelivery_unsubscribe, deliveryCtx, contentTopic.c_str()));

    if (!outcome.success) {
        fprintf(stderr, "DeliveryModuleImpl: Unsubscribe failed for topic: %s, reason: %s\n",
                contentTopic.c_str(), outcome.error.c_str());
    }

    fprintf(stderr, "DeliveryModuleImpl: Unsubscribe completed for topic: %s with success\n", contentTopic.c_str());
    return outcome;
}

std::string DeliveryModuleImpl::version() const {
    std::string moduleVersion = "1.1.0";
    if (!deliveryCtx) {
        fprintf(stderr, "DeliveryModuleImpl: Cannot get version - context not initialized. Call createNode first.\n");
        return moduleVersion + " (liblogosdelivery version unknown, context not initialized)";
    }

    auto liblogosDeliveryVersion = callApiRetValue(
        "get_node_info",
        CALLBACK_TIMEOUT,
        bindApiCall(logosdelivery_get_node_info, deliveryCtx, "Version"));

    if (!liblogosDeliveryVersion.success) {
        fprintf(stderr, "DeliveryModuleImpl: Get node info failed getting version, reason: %s\n",
                liblogosDeliveryVersion.error.c_str());
        return moduleVersion + " (liblogosdelivery version unknown)";
    }

    std::string ver = liblogosDeliveryVersion.value.get<std::string>();
    fprintf(stderr, "DeliveryModuleImpl: Get node info completed for attribute: Version, with success: %s\n", ver.c_str());

    return moduleVersion + " (liblogosdelivery version: " + ver + ")";
}

StdLogosResult DeliveryModuleImpl::getAvailableNodeInfoIDs() {
    fprintf(stderr, "DeliveryModuleImpl::getAvailableNodeInfoIDs called\n");

    if (!deliveryCtx) {
        fprintf(stderr, "DeliveryModuleImpl: Cannot get available node info IDs - context not initialized. Call createNode first.\n");
        return {false, {}, "Context not initialized"};
    }
    auto outcome = callApiRetValue(
        "get_available_node_info_ids",
        CALLBACK_TIMEOUT,
        bindApiCall(logosdelivery_get_available_node_info_ids, deliveryCtx));

    if (!outcome.success) {
        fprintf(stderr, "DeliveryModuleImpl: Get available node info IDs failed, reason: %s\n", outcome.error.c_str());
    }
    return outcome;
}

StdLogosResult DeliveryModuleImpl::getNodeInfo(const std::string& nodeInfoId) {
    fprintf(stderr, "DeliveryModuleImpl::getNodeInfo called with nodeInfoId: %s\n", nodeInfoId.c_str());

    if (!deliveryCtx) {
        fprintf(stderr, "DeliveryModuleImpl: Cannot get node info - context not initialized. Call createNode first.\n");
        return {false, {}, "Context not initialized"};
    }
    auto outcome = callApiRetValue(
        "get_node_info",
        CALLBACK_TIMEOUT,
        bindApiCall(logosdelivery_get_node_info, deliveryCtx, nodeInfoId.c_str()));

    if (!outcome.success) {
        fprintf(stderr, "DeliveryModuleImpl: Get node info failed for ID: %s, reason: %s\n",
                nodeInfoId.c_str(), outcome.error.c_str());
    }

    return outcome;
}

// ============================================================================
// RLN Operations
// ============================================================================

StdLogosResult DeliveryModuleImpl::initLogos(const std::string& apiHandleHex)
{
    bool ok = false;
    quintptr raw = QString::fromStdString(apiHandleHex).toULongLong(&ok, 16);
    if (!ok || raw == 0) {
        fprintf(stderr, "DeliveryModuleImpl::initLogos: invalid api handle: %s\n",
                apiHandleHex.c_str());
        return {false, {}, "invalid api handle"};
    }
    logosAPI = reinterpret_cast<LogosAPI*>(raw);
    fprintf(stderr, "DeliveryModuleImpl: LogosAPI handle installed: %p\n",
            static_cast<void*>(logosAPI));
    return {true, {}};
}

// rln_fetcher is the FFI trampoline registered with liblogosdelivery via
// logosdelivery_set_rln_fetcher. Called from Nim chronos worker threads
// (callRlnFetcherAsync runs fetchers off the chronos main loop).
//
// Threading contract:
// - Always dispatched onto the Qt thread via QueuedConnection (non-blocking
//   enqueue). The Qt thread is NEVER blocked waiting for the RPC to
//   liblogos_rln_module — instead, the typed client's *Async variants fire
//   their callbacks on the Qt event loop when the response arrives.
// - The calling Nim worker thread blocks on a std::promise::get_future()
//   until the async callback fires. This preserves the existing
//   logosdelivery_set_rln_fetcher contract (sync from Nim's view) without
//   holding the Qt event loop hostage during long-running register_member
//   RPCs (5-60s on testnet).
int DeliveryModuleImpl::rln_fetcher(const char* method, const char* params,
    void (*callback)(int, const char*, size_t, void*), void* callbackData, void* fetcherData)
{
    auto* impl = static_cast<DeliveryModuleImpl*>(fetcherData);
    if (!impl || !impl->isContextReady()) {
        if (callback) callback(1, "LogosAPI not available", 21, callbackData);
        return 1;
    }
    // Lazy-fetch from the codegen-emitted LogosModules aggregate. The local
    // `logosAPI` field was set by the legacy initLogos(hex) hook which the
    // universal-codegen provider doesn't auto-invoke; modules().api is the
    // canonical access path for codegen modules.
    if (!impl->logosAPI) {
        impl->logosAPI = impl->modules().api;
    }

    const std::string m = method ? method : "";
    const std::string p = params ? params : "";
    auto promise = std::make_shared<std::promise<QString>>();
    auto future = promise->get_future();

    QMetaObject::invokeMethod(impl->emitRouter(), [impl, m, p, promise]() {
        // Heap-allocate LiblogosRlnModule and capture by shared_ptr into each
        // setValue lambda. The typed client's *Async methods return
        // immediately; their callbacks fire later on the Qt event loop. If
        // `rln` were a stack local, it'd be destroyed when this lambda
        // returns (before the callback fires), causing a use-after-free
        // SIGSEGV from the Nim worker thread.
        auto rln = std::make_shared<LiblogosRlnModule>(impl->logosAPI);

        const QString methodStr = QString::fromStdString(m);
        const QString paramsStr = QString::fromStdString(p);
        auto setValue = [promise, rln](const QString& v) { promise->set_value(v); };

        if (methodStr == "get_valid_roots") {
            rln->get_valid_rootsAsync(paramsStr, setValue);
            return;
        }
        if (methodStr == "get_merkle_proofs") {
            const QStringList parts = paramsStr.split(",");
            if (parts.size() < 2) { setValue({}); return; }
            const QString configAccount = parts[0];
            const QString leafIndicesJson = "[" + parts[1] + "]";
            rln->get_merkle_proofsAsync(configAccount, leafIndicesJson,
                [setValue](QString proofsJson) {
                    const QJsonArray arr = QJsonDocument::fromJson(proofsJson.toUtf8()).array();
                    if (arr.isEmpty()) { setValue({}); return; }
                    setValue(QString::fromUtf8(
                        QJsonDocument(arr[0].toObject()).toJson(QJsonDocument::Compact)));
                });
            return;
        }
        if (methodStr == "generate_identity") {
            rln->generate_identityAsync(paramsStr, setValue);
            return;
        }
        if (methodStr == "register_member") {
            const QJsonDocument paramsDoc = QJsonDocument::fromJson(paramsStr.toUtf8());
            if (!paramsDoc.isObject()) { setValue({}); return; }
            const QJsonObject o = paramsDoc.object();
            const QString cfg = o["configAccountId"].toString();
            const QString holder = o["userHoldingAccountId"].toString();
            const QString idCommit = o["idCommitment"].toString();
            const int rateLimit = o["rateLimit"].toInt(100);
            if (cfg.isEmpty() || holder.isEmpty() || idCommit.isEmpty()) {
                setValue({}); return;
            }
            // register_member's internal pre-check + wallet send takes
            // ~5-30s+ on testnet. Default 20s typed-client timeout aborts
            // mid-call when peers queue serially. Bump to 180s.
            rln->register_memberAsync(cfg, holder, idCommit, rateLimit, setValue,
                                       Timeout(180000));
            return;
        }
        if (methodStr == "is_member_registered") {
            const QJsonDocument paramsDoc = QJsonDocument::fromJson(paramsStr.toUtf8());
            if (!paramsDoc.isObject()) { setValue({}); return; }
            const QJsonObject o = paramsDoc.object();
            const QString cfg = o["configAccountId"].toString();
            const QString idCommit = o["idCommitment"].toString();
            if (cfg.isEmpty() || idCommit.isEmpty()) { setValue({}); return; }
            rln->is_member_registeredAsync(cfg, idCommit, setValue);
            return;
        }
        setValue({});
    }, Qt::QueuedConnection);

    const QString result = future.get();
    if (result.isEmpty()) {
        if (callback) callback(1, "rln_fetcher failed", 18, callbackData);
        return 1;
    }
    const QByteArray utf8 = result.toUtf8();
    if (callback) callback(0, utf8.constData(), utf8.size(), callbackData);
    return 0;
}

StdLogosResult DeliveryModuleImpl::setRlnConfig(const std::string& configAccountId, int64_t leafIndex)
{
    if (!deliveryCtx) {
        fprintf(stderr, "DeliveryModuleImpl: Cannot set RLN config - context not initialized\n");
        return {false, {}, "Context not initialized"};
    }

    logosdelivery_set_rln_fetcher(deliveryCtx, rln_fetcher, this);
    logosdelivery_set_rln_config(deliveryCtx, configAccountId.c_str(),
                                  static_cast<int>(leafIndex));

    // Event subscription is deferred to avoid blocking the -c call.
    // The RLN module broadcasts roots/proofs on a timer; subscribe after 5s.
    if (logosAPI) {
        void* ctx = deliveryCtx;
        auto* api = logosAPI;
        QTimer::singleShot(5000, [ctx, api]() {
            auto* rlnConsumer = new LogosAPIConsumer("liblogos_rln_module", "delivery_module",
                                                      api->getTokenManager());
            LogosObject* rlnReplica = rlnConsumer->requestObject("liblogos_rln_module");
            if (rlnReplica) {
                rlnConsumer->onEvent(rlnReplica, "valid_roots",
                    [ctx](const QString&, const QVariantList& data) {
                        if (data.isEmpty()) return;
                        QByteArray utf8 = data[0].toString().toUtf8();
                        if (!utf8.isEmpty())
                            logosdelivery_push_roots(ctx, utf8.constData());
                    });
                rlnConsumer->onEvent(rlnReplica, "merkle_proof",
                    [ctx](const QString&, const QVariantList& data) {
                        if (data.isEmpty()) return;
                        QByteArray utf8 = data[0].toString().toUtf8();
                        if (!utf8.isEmpty())
                            logosdelivery_push_proof(ctx, utf8.constData());
                    });
                qDebug() << "DeliveryModuleImpl: Subscribed to RLN module events";
            } else {
                qWarning() << "DeliveryModuleImpl: Could not get RLN module replica";
            }
        });
    }

    qDebug() << "DeliveryModuleImpl: RLN config set, account:"
             << QString::fromStdString(configAccountId) << "leaf:" << leafIndex;
    return {true, {}};
}

StdLogosResult DeliveryModuleImpl::selfRegisterRln(const std::string& configAccountIdStd,
                                                    const std::string& walletAccountIdStd,
                                                    int64_t rateLimit)
{
    if (!isContextReady()) {
        return {false, {}, "module context not ready"};
    }
    if (!logosAPI) {
        logosAPI = modules().api;  // lazy wire, see rln_fetcher for rationale
    }
    if (!logosAPI) {
        return {false, {}, "logosAPI not initialized"};
    }

    const QString configAccountId = QString::fromStdString(configAccountIdStd);
    const QString walletAccountId = QString::fromStdString(walletAccountIdStd);

    auto* rlnClient = logosAPI->getClient("liblogos_rln_module");
    if (!rlnClient) {
        return {false, {}, "RLN module not available"};
    }

    QByteArray seedBytes(32, 0);
    for (int i = 0; i < 32; ++i)
        seedBytes[i] = static_cast<char>(QRandomGenerator::global()->generate() & 0xFF);
    QString seed = QString::fromLatin1(seedBytes.toHex());

    qDebug() << "selfRegisterRln: generating identity with seed" << seed.left(16) << "...";
    QVariant genResult = rlnClient->invokeRemoteMethod(
        "liblogos_rln_module", "generate_identity", QVariant(seed));
    QString genJson = genResult.toString();
    if (genJson.isEmpty()) {
        return {false, {}, "generate_identity failed"};
    }

    QJsonDocument genDoc = QJsonDocument::fromJson(genJson.toUtf8());
    QString idCommitment = genDoc.object()["id_commitment"].toString();
    QString idSecretHash = genDoc.object()["id_secret_hash"].toString();
    if (idCommitment.isEmpty() || idSecretHash.isEmpty()) {
        return {false, {}, "failed to parse identity"};
    }
    qDebug() << "selfRegisterRln: identity generated, commitment:"
             << idCommitment.left(16) << "...";

    // register_member returns immediately after the tx is submitted (does not
    // wait for confirmation); callers needing to know when the tx lands should
    // poll is_member_registered. The chat/delivery side picks up the confirmed
    // state via valid_roots / get_merkle_proofs once the tree advances.
    qDebug() << "selfRegisterRln: registering member...";
    QVariant regResult = rlnClient->invokeRemoteMethod(
        "liblogos_rln_module", "register_member",
        QVariant(configAccountId), QVariant(walletAccountId),
        QVariant(idCommitment), QVariant(static_cast<qlonglong>(rateLimit)));
    QString regJson = regResult.toString();
    if (regJson.isEmpty()) {
        return {false, {}, "register_member failed"};
    }

    QJsonDocument regDoc = QJsonDocument::fromJson(regJson.toUtf8());
    int leafIndex = static_cast<int>(regDoc.object()["leaf_index"].toDouble());
    qDebug() << "selfRegisterRln: registered at leaf" << leafIndex;

    auto wireResult = setRlnConfig(configAccountIdStd, leafIndex);
    if (!wireResult.success) {
        return wireResult;
    }

    // Install the seed (not idSecretHash) so the Nim side can regenerate the
    // full credential (idTrapdoor, idNullifier, idSecretHash, idCommitment)
    // via membershipKeyGen(seed).
    if (deliveryCtx) {
        logosdelivery_set_rln_identity(deliveryCtx, seed.toUtf8().constData());
    }

    nlohmann::json out;
    out["id_secret_hash"] = idSecretHash.toStdString();
    out["id_commitment"] = idCommitment.toStdString();
    out["leaf_index"] = leafIndex;
    return {true, out};
}

StdLogosResult DeliveryModuleImpl::selfRegisterRlnJson(const std::string& argsJson) {
    nlohmann::json j;
    try {
        j = nlohmann::json::parse(argsJson);
    } catch (const std::exception& e) {
        return {false, {}, std::string("selfRegisterRlnJson: invalid JSON: ") + e.what()};
    }
    if (!j.is_object()) {
        return {false, {}, "selfRegisterRlnJson: payload must be an object"};
    }
    if (!j.contains("config") || !j["config"].is_string()
            || !j.contains("wallet") || !j["wallet"].is_string()
            || !j.contains("rate") || !j["rate"].is_number_integer()) {
        return {false, {}, "selfRegisterRlnJson: payload requires {config:str, wallet:str, rate:int}"};
    }
    return selfRegisterRln(j["config"].get<std::string>(),
                           j["wallet"].get<std::string>(),
                           j["rate"].get<int64_t>());
}

StdLogosResult DeliveryModuleImpl::getAvailableConfigs() {
    fprintf(stderr, "DeliveryModuleImpl::getAvailableConfigs called\n");

    if (!deliveryCtx) {
        fprintf(stderr, "DeliveryModuleImpl: Cannot get available configs - context not initialized. Call createNode first.\n");
        return {false, {}, "Context not initialized"};
    }
    auto outcome = callApiRetValue(
        "get_available_configs",
        CALLBACK_TIMEOUT,
        bindApiCall(logosdelivery_get_available_configs, deliveryCtx));

    if (!outcome.success) {
        fprintf(stderr, "DeliveryModuleImpl: Get available configs failed, reason: %s\n", outcome.error.c_str());
    }

    return outcome;
}
