#include "delivery_module_plugin.h"
#include <QDebug>
#include <QVariantList>
#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QTimer>
#include <semaphore>

#include "api_call_handler.h"
// Include the liblogosdelivery header from logos-delivery
// liblogosdelivery provides a high-level message-delivery API
extern "C" {
#include <liblogosdelivery.h>
void logosdelivery_push_roots(void* ctx, const char* rootsJson);
void logosdelivery_push_proof(void* ctx, const char* proofJson);
}

DeliveryModulePlugin::DeliveryModulePlugin() : deliveryCtx(nullptr)
{
    qDebug() << "DeliveryModulePlugin: Initializing...";
    qDebug() << "DeliveryModulePlugin: Initialized successfully";
}

DeliveryModulePlugin::~DeliveryModulePlugin() 
{
    // Clean up resources, this is not done in PluginInterface destructor
    if (logosAPI) {
        delete logosAPI;
        logosAPI = nullptr;
    }
    
    // Clean up delivery context if it exists
    if (deliveryCtx) {
        logosdelivery_destroy(deliveryCtx, nullptr, nullptr);
        deliveryCtx = nullptr;
    }
}

void DeliveryModulePlugin::emitEvent(const QString& eventName, const QVariantList& data) {
    if (!logosAPI) {
        qWarning() << "DeliveryModulePlugin: LogosAPI not available, cannot emit" << eventName;
        return;
    }

    LogosAPIClient* client = logosAPI->getClient("delivery_module");
    if (!client) {
        qWarning() << "DeliveryModulePlugin: Failed to get delivery_module client for event" << eventName;
        return;
    }

    client->onEventResponse(this, eventName, data);
}

// Static callback function for liblogosdelivery events, this one is one time registered
// on initialization and will be called for all events from the Nim FFI side.
void DeliveryModulePlugin::event_callback(int callerRet, const char* msg, size_t len, void* userData)
{
    qDebug() << "DeliveryModulePlugin::event_callback called with ret:" << callerRet;

    DeliveryModulePlugin* plugin = static_cast<DeliveryModulePlugin*>(userData);
    if (!plugin) {
        qWarning() << "DeliveryModulePlugin::event_callback: Invalid userData";
        return;
    }

    if (msg && len > 0) {
        QString message = QString::fromUtf8(msg, len);
        qDebug() << "DeliveryModulePlugin::event_callback message:" << message;
        
        // Parse JSON to determine event type
        QJsonDocument doc = QJsonDocument::fromJson(message.toUtf8());
        if (!doc.isObject()) {
            qWarning() << "DeliveryModulePlugin::event_callback: Invalid JSON";
            return;
        }
        
        QJsonObject jsonObj = doc.object();
        QString eventType = jsonObj["eventType"].toString();
        QString timestamp = QDateTime::currentDateTime().toString(Qt::ISODate);
        
        if (eventType == "message_sent") {
            // MessageSentEvent: requestId, messageHash
            QVariantList eventData;
            eventData << jsonObj["requestId"].toString();
            eventData << jsonObj["messageHash"].toString();
            eventData << timestamp;
            plugin->emitEvent("messageSent", eventData);
            
        } else if (eventType == "message_error") {
            // MessageErrorEvent: requestId, messageHash, error
            QVariantList eventData;
            eventData << jsonObj["requestId"].toString();
            eventData << jsonObj["messageHash"].toString();
            eventData << jsonObj["error"].toString();
            eventData << timestamp;
            plugin->emitEvent("messageError", eventData);
            
        } else if (eventType == "message_propagated") {
            // MessagePropagatedEvent: requestId, messageHash
            QVariantList eventData;
            eventData << jsonObj["requestId"].toString();
            eventData << jsonObj["messageHash"].toString();
            eventData << timestamp;
            plugin->emitEvent("messagePropagated", eventData);
            
        } else if (eventType == "message_received") {
            // MessageReceivedEvent: messageHash, message (WakuMessage)
            QJsonObject msgObj = jsonObj["message"].toObject();
            QVariantList eventData;
            eventData << jsonObj["messageHash"].toString();
            eventData << msgObj["contentTopic"].toString();
            eventData << msgObj["payload"].toString();
            eventData << QString::number(msgObj["timestamp"].toDouble(), 'f', 0);
            plugin->emitEvent("messageReceived", eventData);

        } else if (eventType == "connection_status_change") {
            QVariantList eventData;
            eventData << jsonObj["connectionStatus"].toString();
            eventData << timestamp;
            plugin->emitEvent("connectionStateChanged", eventData);
            
        } else {
            qWarning() << "DeliveryModulePlugin::event_callback: Unknown event type:" << eventType;
        }
    }
}

void DeliveryModulePlugin::initLogos(LogosAPI* logosAPIInstance) {
    if (logosAPI) {
        delete logosAPI;
    }
    logosAPI = logosAPIInstance;
}

bool DeliveryModulePlugin::createNode(const QString &cfg)
{
    qDebug() << "DeliveryModulePlugin::createNode called with cfg:" << cfg;
    
    // Convert QString to UTF-8 byte array
    QByteArray cfgUtf8 = cfg.toUtf8();
    
    // Create semaphore and callback context for synchronous operation
    // Callback is only called in failure case
    struct CallbackContext {
        std::binary_semaphore* sem;
        bool callbackInvoked;
    };
    
    std::binary_semaphore sem(0);
    CallbackContext ctx{&sem, false};
    
    // Lambda callback that will be called only on failure (when deliveryCtx is nullptr)
    auto callback = +[](int callerRet, const char* msg, size_t len, void* userData) {
        qDebug() << "DeliveryModulePlugin::createNode callback called with ret:" << callerRet;
        
        CallbackContext* ctx = static_cast<CallbackContext*>(userData);
        if (!ctx) {
            qWarning() << "DeliveryModulePlugin::createNode callback: Invalid userData";
            return;
        }
        
        if (msg && len > 0) {
            QString message = QString::fromUtf8(msg, len);
            qDebug() << "DeliveryModulePlugin::createNode callback message:" << message;
        }
        
        ctx->callbackInvoked = true;
        
        // Release semaphore to unblock the createNode method
        ctx->sem->release();
    };
    
    // Call logosdelivery_create_node with the configuration
    // Important: Keep deliveryCtx assignment from the call
    deliveryCtx = logosdelivery_create_node(cfgUtf8.constData(), callback, &ctx);
    
    // If deliveryCtx is nullptr, callback will be invoked with error details
    if (!deliveryCtx) {
        qDebug() << "DeliveryModulePlugin: Waiting for createNode error callback...";
        
        // Wait for callback to complete with timeout
        if (!sem.try_acquire_for(CALLBACK_TIMEOUT)) {
            qWarning() << "DeliveryModulePlugin: Timeout waiting for createNode callback";
            return false;
        }
        
        qWarning() << "DeliveryModulePlugin: Failed to create Messaging context";
        return false;
    }
    
    // Success case - deliveryCtx is valid, callback won't be called
    qDebug() << "DeliveryModulePlugin: Messaging context created successfully";
    
    // Set up event callback
    logosdelivery_set_event_callback(deliveryCtx, event_callback, this);
    return true;
}

bool DeliveryModulePlugin::start()
{
    qDebug() << "DeliveryModulePlugin::start called";

    if (!deliveryCtx) {
        qWarning() << "DeliveryModulePlugin: Cannot start Messaging - context not initialized. Call createNode first.";
        return false;
    }

    // Fire-and-forget: the FFI thread starts the Waku node asynchronously.
    // We cannot block here because the start_node handler awaits peer connections
    // which may take longer than logoscore's -c IPC timeout (20s).
    auto callback = +[](int callerRet, const char* msg, size_t len, void*) {
        if (callerRet != 0) {
            QString message = (msg && len > 0) ? QString::fromUtf8(msg, len) : "unknown error";
            qWarning() << "DeliveryModulePlugin: Async start completed with error:" << message;
        } else {
            qDebug() << "DeliveryModulePlugin: Async start completed successfully";
        }
    };

    int ret = logosdelivery_start_node(deliveryCtx, callback, nullptr);
    if (ret != 0) {
        qWarning() << "DeliveryModulePlugin: Failed to initiate start";
        return false;
    }

    qDebug() << "DeliveryModulePlugin: Start initiated (async)";
    return true;
}

bool DeliveryModulePlugin::stop()
{
    qDebug() << "DeliveryModulePlugin::stop called";
    
    if (!deliveryCtx) {
        qWarning() << "DeliveryModulePlugin: Cannot stop Messaging - context not initialized.";
        return false;
    }
    
    auto outcome = callApiRetVoid(
        "stop",
        CALLBACK_TIMEOUT,
        bindApiCall(logosdelivery_stop_node, deliveryCtx));

    if (outcome.isErr()) {
        qWarning() << "DeliveryModulePlugin: Stop failed:" << outcome.error();
        return false;
    }

    qDebug() << "DeliveryModulePlugin: Messaging stop completed with success: true";
    return true;
}
QExpected<QString> DeliveryModulePlugin::send(const QString &contentTopic, const QString &payload)
{
    qDebug() << "DeliveryModulePlugin::send called with contentTopic:" << contentTopic;
    qDebug() << "DeliveryModulePlugin::send payload:" << payload;
    
    if (!deliveryCtx) {
        qWarning() << "DeliveryModulePlugin: Cannot send message - context not initialized. Call createNode first.";
        return QExpected<QString>::err("Context not initialized");
    }
    
    // Construct JSON message according to logosdelivery_send API
    // The payload should be base64-encoded as per the API spec
    QJsonObject messageObj;
    messageObj["contentTopic"] = contentTopic;
    messageObj["payload"] = QString::fromUtf8(payload.toUtf8().toBase64());
    messageObj["ephemeral"] = false;
    
    QJsonDocument doc(messageObj);
    QByteArray messageJson = doc.toJson(QJsonDocument::Compact);
    
    auto outcome = callApiRetValue<QString>(
        "send",
        CALLBACK_TIMEOUT,
        bindApiCall(logosdelivery_send, deliveryCtx, messageJson.constData()));

    if (outcome.isErr()) {
        qWarning() << "DeliveryModulePlugin: Send failed for topic:" << contentTopic << ", reason:" << outcome.error();
        return QExpected<QString>::err(outcome.error());
    }

    const QString responseMessage = outcome.value();
    qDebug() << "DeliveryModulePlugin: Send initiated for topic:" << contentTopic << ", with success: true";
    return QExpected<QString>::ok(responseMessage);
}

bool DeliveryModulePlugin::subscribe(const QString &contentTopic)
{
    qDebug() << "DeliveryModulePlugin::subscribe called with contentTopic:" << contentTopic;
    
    if (!deliveryCtx) {
        qWarning() << "DeliveryModulePlugin: Cannot subscribe - context not initialized. Call createNode first.";
        return false;
    }
    
    // Convert QString to UTF-8 byte array
    QByteArray topicUtf8 = contentTopic.toUtf8();
    
    auto outcome = callApiRetVoid(
        "subscribe",
        CALLBACK_TIMEOUT,
        bindApiCall(logosdelivery_subscribe, deliveryCtx, topicUtf8.constData()));

    if (outcome.isErr()) {
        qWarning() << "DeliveryModulePlugin: Subscribe failed for topic:" << contentTopic << ", reason:" << outcome.error();
        return false;
    }

    qDebug() << "DeliveryModulePlugin: Subscribe completed for topic:" << contentTopic << " with success: true";
    return true;
}

bool DeliveryModulePlugin::unsubscribe(const QString &contentTopic)
{
    qDebug() << "DeliveryModulePlugin::unsubscribe called with contentTopic:" << contentTopic;
    
    if (!deliveryCtx) {
        qWarning() << "DeliveryModulePlugin: Cannot unsubscribe - context not initialized.";
        return false;
    }
    
    // Convert QString to UTF-8 byte array
    QByteArray topicUtf8 = contentTopic.toUtf8();
    
    auto outcome = callApiRetVoid(
        "unsubscribe",
        CALLBACK_TIMEOUT,
        bindApiCall(logosdelivery_unsubscribe, deliveryCtx, topicUtf8.constData()));

    if (outcome.isErr()) {
        qWarning() << "DeliveryModulePlugin: Unsubscribe failed for topic:" << contentTopic << ", reason:" << outcome.error();
        return false;
    }

    qDebug() << "DeliveryModulePlugin: Unsubscribe completed for topic:" << contentTopic << " with success: true";
    return true;
}

QString DeliveryModulePlugin::version() const {
    QString moduleVersion = "1.0.0";
    if (!deliveryCtx) {
        qWarning() << "DeliveryModulePlugin: Cannot subscribe - context not initialized. Call createNode first.";
        return moduleVersion + " (liblogosdelivery version unknown, context not initialized)";
    }

    auto attributeName = "Version";
    auto liblogosDeliveryVersion = callApiRetValue<QString>(
        "get_node_info",
        CALLBACK_TIMEOUT,
        bindApiCall(logosdelivery_get_node_info, deliveryCtx, attributeName));

    if (liblogosDeliveryVersion.isErr()) {
        qWarning() << "DeliveryModulePlugin: Get node info failed getting version, reason:" <<
            liblogosDeliveryVersion.error();
        return moduleVersion + " (liblogosdelivery version unknown)";
    }

    const QString version = liblogosDeliveryVersion.value();
    qDebug() << "DeliveryModulePlugin: Get node info completed for attribute:" <<
        attributeName << ", with success: " << version;

    return moduleVersion + " (liblogosdelivery version: " + version + ")";
}

QString DeliveryModulePlugin::getAvailableNodeInfoIDs() {
    auto outcome = callApiRetValue<QString>(
        "get_available_node_info_ids",
        CALLBACK_TIMEOUT,
        bindApiCall(logosdelivery_get_available_node_info_ids, deliveryCtx));

    if (outcome.isErr()) {
        qWarning() << "DeliveryModulePlugin: Get available node info IDs failed, reason:" << outcome.error();
        return QString();
    }

    return outcome.value();
}

QString DeliveryModulePlugin::getNodeInfo(const QString &nodeInfoId) {
    auto outcome = callApiRetValue<QString>(
        "get_node_info",
        CALLBACK_TIMEOUT,
        bindApiCall(logosdelivery_get_node_info, deliveryCtx, nodeInfoId.toUtf8().constData()));

    if (outcome.isErr()) {
        qWarning() << "DeliveryModulePlugin: Get node info failed for ID:" << nodeInfoId <<
            ", reason:" << outcome.error();
        return QString();
    }

    return outcome.value();
}

QString DeliveryModulePlugin::getAvailableConfigs() {
    auto outcome = callApiRetValue<QString>(
        "get_available_configs",
        CALLBACK_TIMEOUT,
        bindApiCall(logosdelivery_get_available_configs, deliveryCtx));

    if (outcome.isErr()) {
        qWarning() << "DeliveryModulePlugin: Get available configs failed, reason:" << outcome.error();
        return QString();
    }

    return outcome.value();
}

int DeliveryModulePlugin::rln_fetcher(const char* method, const char* params,
    void (*callback)(int, const char*, size_t, void*), void* callbackData, void* fetcherData)
{
    auto* plugin = static_cast<DeliveryModulePlugin*>(fetcherData);
    if (!plugin || !plugin->logosAPI) {
        if (callback) callback(1, "LogosAPI not available", 21, callbackData);
        return 1;
    }

    auto* rlnClient = plugin->logosAPI->getClient("liblogos_rln_module");
    if (!rlnClient) {
        if (callback) callback(1, "RLN module not available", 24, callbackData);
        return 1;
    }

    QString methodStr(method);
    QString paramsStr(params);

    if (methodStr == "get_valid_roots") {
        QVariant result = rlnClient->invokeRemoteMethod(
            "liblogos_rln_module", "get_valid_roots", QVariant(paramsStr));
        QByteArray utf8 = result.toString().toUtf8();
        if (callback) callback(0, utf8.constData(), utf8.size(), callbackData);
        return 0;
    }

    if (methodStr == "get_merkle_proofs") {
        QStringList parts = paramsStr.split(",");
        if (parts.size() < 2) {
            if (callback) callback(1, "Expected configAccountId,leafIndex", 35, callbackData);
            return 1;
        }
        QString configAccount = parts[0];
        QString leafIndicesJson = "[" + parts[1] + "]";
        QVariant result = rlnClient->invokeRemoteMethod(
            "liblogos_rln_module", "get_merkle_proofs",
            QVariant(configAccount), QVariant(leafIndicesJson));
        QString proofsJson = result.toString();

        QJsonArray arr = QJsonDocument::fromJson(proofsJson.toUtf8()).array();
        if (arr.isEmpty()) {
            if (callback) callback(1, "Empty proof array", 17, callbackData);
            return 1;
        }
        QByteArray singleProof = QJsonDocument(arr[0].toObject()).toJson(QJsonDocument::Compact);
        if (callback) callback(0, singleProof.constData(), singleProof.size(), callbackData);
        return 0;
    }

    if (callback) callback(1, "Unknown method", 14, callbackData);
    return 1;
}

bool DeliveryModulePlugin::setRlnConfig(const QString& configAccountId, int leafIndex)
{
    if (!deliveryCtx) {
        qWarning() << "DeliveryModulePlugin: Cannot set RLN config - context not initialized";
        return false;
    }

    logosdelivery_set_rln_fetcher(deliveryCtx, rln_fetcher, this);
    logosdelivery_set_rln_config(deliveryCtx, configAccountId.toUtf8().constData(), leafIndex);

    // Event subscription is deferred to avoid blocking the -c call
    // The RLN module broadcasts roots/proofs on a timer; we subscribe after a delay
    if (logosAPI) {
        void* ctx = deliveryCtx;
        auto* api = logosAPI;
        QTimer::singleShot(5000, [ctx, api]() {
            auto* rlnConsumer = new LogosAPIConsumer("liblogos_rln_module", "delivery_module",
                                                      api->getTokenManager());
            QObject* rlnReplica = rlnConsumer->requestObject("liblogos_rln_module");
            if (rlnReplica) {
                rlnConsumer->onEvent(rlnReplica, rlnConsumer, "valid_roots",
                    [ctx](const QString& eventName, const QVariantList& data) {
                        if (data.isEmpty()) return;
                        QByteArray utf8 = data[0].toString().toUtf8();
                        if (!utf8.isEmpty())
                            logosdelivery_push_roots(ctx, utf8.constData());
                    });
                rlnConsumer->onEvent(rlnReplica, rlnConsumer, "merkle_proof",
                    [ctx](const QString& eventName, const QVariantList& data) {
                        if (data.isEmpty()) return;
                        QByteArray utf8 = data[0].toString().toUtf8();
                        if (!utf8.isEmpty())
                            logosdelivery_push_proof(ctx, utf8.constData());
                    });
                qDebug() << "DeliveryModulePlugin: Subscribed to RLN module events";
            } else {
                qWarning() << "DeliveryModulePlugin: Could not get RLN module replica";
            }
        });
    }

    qDebug() << "DeliveryModulePlugin: RLN config set, account:" << configAccountId << "leaf:" << leafIndex;
    return true;
}

bool DeliveryModulePlugin::sendTest(const QString& contentTopic, const QString& payload)
{
    if (!deliveryCtx) {
        qWarning() << "DeliveryModulePlugin: sendTest - context not initialized";
        return false;
    }

    void* ctx = deliveryCtx;
    QTimer::singleShot(5000, [ctx, contentTopic, payload]() {
        QJsonObject messageObj;
        messageObj["contentTopic"] = contentTopic;
        messageObj["payload"] = QString::fromUtf8(payload.toUtf8().toBase64());
        messageObj["ephemeral"] = false;

        QJsonDocument doc(messageObj);
        QByteArray messageJson = doc.toJson(QJsonDocument::Compact);

        auto callback = +[](int callerRet, const char* msg, size_t len, void*) {
            if (callerRet != 0) {
                QString message = (msg && len > 0) ? QString::fromUtf8(msg, len) : "unknown error";
                qWarning() << "sendTest result: error -" << message;
            } else {
                QString message = (msg && len > 0) ? QString::fromUtf8(msg, len) : "";
                qDebug() << "sendTest result: success, requestId:" << message;
            }
        };

        qDebug() << "sendTest: sending deferred message";
        logosdelivery_send(ctx, callback, nullptr, messageJson.constData());
    });
    return true;
}

