#pragma once
#include <QString>
#include <QVariant>
#include <QStringList>
#include <QJsonArray>
#include <QObject>
#include <QPointer>
#include <functional>
#include <utility>
#include "logos_api.h"
#include "logos_api_client.h"

class CoreManager {
public:
    explicit CoreManager(LogosAPI* api);

    using RawEventCallback = std::function<void(const QString&, const QVariantList&)>;
    using EventCallback = std::function<void(const QVariantList&)>;

    bool on(const QString& eventName, RawEventCallback callback);
    bool on(const QString& eventName, EventCallback callback);
    void setEventSource(QObject* source);
    QObject* eventSource() const;
    void trigger(const QString& eventName);
    void trigger(const QString& eventName, const QVariantList& data);
    template<typename... Args>
    void trigger(const QString& eventName, Args&&... args) {
        trigger(eventName, packVariantList(std::forward<Args>(args)...));
    }
    void trigger(const QString& eventName, QObject* source, const QVariantList& data);
    template<typename... Args>
    void trigger(const QString& eventName, QObject* source, Args&&... args) {
        trigger(eventName, source, packVariantList(std::forward<Args>(args)...));
    }

    void initialize(int argc, char* argv[]);
    void setPluginsDirectory(const QString& directory);
    void start();
    void cleanup();
    QStringList getLoadedPlugins();
    QJsonArray getKnownPlugins();
    QJsonArray getPluginMethods(const QString& pluginName);
    void helloWorld();
    bool loadPlugin(const QString& pluginName);
    bool unloadPlugin(const QString& pluginName);
    QString processPlugin(const QString& filePath);

private:
    QObject* ensureReplica();
    template<typename... Args>
    static QVariantList packVariantList(Args&&... args) {
        QVariantList list;
        list.reserve(sizeof...(Args));
        using Expander = int[];
        (void)Expander{0, (list.append(QVariant::fromValue(std::forward<Args>(args))), 0)...};
        return list;
    }
    LogosAPI* m_api;
    LogosAPIClient* m_client;
    QString m_moduleName;
    QPointer<QObject> m_eventReplica;
    QPointer<QObject> m_eventSource;
};
