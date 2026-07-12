// Typed client for liblogos_rln_module — header-only.
//
// Mirrors what logos-cpp-generator's --lidl mode would emit
// (see cpp-generator/experimental/lidl_gen_client.cpp). Hand-written
// because the underlying plugin uses the Qt-plugin pattern with
// Q_INVOKABLE methods (dispatched via Qt MOC + invokeRemoteMethod)
// rather than Universal Module shape.
//
// Consumers (e.g. chat_module_plugin) include this and call:
//     LiblogosRlnModule rln(logosApi);
//     rln.register_memberAsync(cfg, holder, idCommitment, rateLimit,
//                              [](QString result) { ... });
//     rln.on("valid_roots", [](const QVariantList& data) { ... });

#pragma once

#include <QString>
#include <QVariant>
#include <QVariantList>
#include <functional>

#include "logos_api.h"
#include "logos_api_client.h"
#include "logos_mode.h"
#include "logos_object.h"

class LiblogosRlnModule {
public:
    explicit LiblogosRlnModule(LogosAPI* api)
        : m_api(api)
        , m_client(api ? api->getClient("liblogos_rln_module") : nullptr)
        , m_moduleName(QStringLiteral("liblogos_rln_module"))
    {}

    using RawEventCallback = std::function<void(const QString&, const QVariantList&)>;
    using EventCallback = std::function<void(const QVariantList&)>;

    bool on(const QString& eventName, RawEventCallback callback) {
        if (!callback || !m_client) return false;
        LogosObject* origin = ensureReplica();
        if (!origin) return false;
        m_client->onEvent(origin, eventName, callback);
        return true;
    }
    bool on(const QString& eventName, EventCallback callback) {
        if (!callback) return false;
        return on(eventName, [callback](const QString&, const QVariantList& data) {
            callback(data);
        });
    }

    // ----- sync wrappers ------------------------------------------------

    QString get_valid_roots(const QString& rln_account_id_hex) {
        if (!m_client) return {};
        QVariant r = m_client->invokeRemoteMethod(m_moduleName, "get_valid_roots", rln_account_id_hex);
        return r.toString();
    }
    void start_root_broadcast(const QString& rln_account_id) {
        if (!m_client) return;
        m_client->invokeRemoteMethod(m_moduleName, "start_root_broadcast", rln_account_id);
    }
    QString get_merkle_proofs(const QString& config_account_id, const QString& leaf_indices_json) {
        if (!m_client) return {};
        QVariant r = m_client->invokeRemoteMethod(m_moduleName, "get_merkle_proofs",
                                                  config_account_id, leaf_indices_json);
        return r.toString();
    }
    void start_merkle_proof_broadcast(const QString& config_account_id, int leaf_index) {
        if (!m_client) return;
        m_client->invokeRemoteMethod(m_moduleName, "start_merkle_proof_broadcast",
                                     config_account_id, leaf_index);
    }
    QString generate_identity(const QString& seed_or_wallet_account_id) {
        if (!m_client) return {};
        QVariant r = m_client->invokeRemoteMethod(m_moduleName, "generate_identity",
                                                  seed_or_wallet_account_id);
        return r.toString();
    }
    QString compute_rate_commitment(const QString& id_commitment_hex, int rate_limit) {
        if (!m_client) return {};
        QVariant r = m_client->invokeRemoteMethod(m_moduleName, "compute_rate_commitment",
                                                  id_commitment_hex, rate_limit);
        return r.toString();
    }
    QString register_member(const QString& config_account_id,
                            const QString& user_holding_account_id,
                            const QString& id_commitment_hex,
                            int rate_limit) {
        if (!m_client) return {};
        QVariant r = m_client->invokeRemoteMethod(m_moduleName, "register_member",
                                                  config_account_id, user_holding_account_id,
                                                  id_commitment_hex, rate_limit);
        return r.toString();
    }
    QString is_member_registered(const QString& config_account_id, const QString& id_commitment_hex) {
        if (!m_client) return {};
        QVariant r = m_client->invokeRemoteMethod(m_moduleName, "is_member_registered",
                                                  config_account_id, id_commitment_hex);
        return r.toString();
    }

    // ----- async wrappers -----------------------------------------------
    // Use these from the Qt thread; callback fires on the Qt event loop.

    void get_valid_rootsAsync(const QString& rln_account_id_hex,
                              std::function<void(QString)> callback,
                              Timeout timeout = Timeout()) {
        if (!callback || !m_client) { if (callback) callback({}); return; }
        m_client->invokeRemoteMethodAsync(m_moduleName, "get_valid_roots",
            QVariantList() << rln_account_id_hex,
            [callback](QVariant v) { callback(v.toString()); }, timeout);
    }
    void get_merkle_proofsAsync(const QString& config_account_id,
                                const QString& leaf_indices_json,
                                std::function<void(QString)> callback,
                                Timeout timeout = Timeout()) {
        if (!callback || !m_client) { if (callback) callback({}); return; }
        m_client->invokeRemoteMethodAsync(m_moduleName, "get_merkle_proofs",
            QVariantList{config_account_id, leaf_indices_json},
            [callback](QVariant v) { callback(v.toString()); }, timeout);
    }
    void generate_identityAsync(const QString& seed_or_wallet_account_id,
                                std::function<void(QString)> callback,
                                Timeout timeout = Timeout()) {
        if (!callback || !m_client) { if (callback) callback({}); return; }
        m_client->invokeRemoteMethodAsync(m_moduleName, "generate_identity",
            QVariantList() << seed_or_wallet_account_id,
            [callback](QVariant v) { callback(v.toString()); }, timeout);
    }
    void compute_rate_commitmentAsync(const QString& id_commitment_hex,
                                      int rate_limit,
                                      std::function<void(QString)> callback,
                                      Timeout timeout = Timeout()) {
        if (!callback || !m_client) { if (callback) callback({}); return; }
        m_client->invokeRemoteMethodAsync(m_moduleName, "compute_rate_commitment",
            QVariantList{id_commitment_hex, rate_limit},
            [callback](QVariant v) { callback(v.toString()); }, timeout);
    }
    void register_memberAsync(const QString& config_account_id,
                              const QString& user_holding_account_id,
                              const QString& id_commitment_hex,
                              int rate_limit,
                              std::function<void(QString)> callback,
                              Timeout timeout = Timeout()) {
        if (!callback || !m_client) { if (callback) callback({}); return; }
        m_client->invokeRemoteMethodAsync(m_moduleName, "register_member",
            QVariantList{config_account_id, user_holding_account_id,
                         id_commitment_hex, rate_limit},
            [callback](QVariant v) { callback(v.toString()); }, timeout);
    }
    void is_member_registeredAsync(const QString& config_account_id,
                                   const QString& id_commitment_hex,
                                   std::function<void(QString)> callback,
                                   Timeout timeout = Timeout()) {
        if (!callback || !m_client) { if (callback) callback({}); return; }
        m_client->invokeRemoteMethodAsync(m_moduleName, "is_member_registered",
            QVariantList{config_account_id, id_commitment_hex},
            [callback](QVariant v) { callback(v.toString()); }, timeout);
    }

private:
    LogosObject* ensureReplica() {
        if (!m_eventReplica && m_client) {
            m_eventReplica = m_client->requestObject(m_moduleName);
        }
        return m_eventReplica;
    }

    LogosAPI* m_api = nullptr;
    LogosAPIClient* m_client = nullptr;
    QString m_moduleName;
    LogosObject* m_eventReplica = nullptr;
};
