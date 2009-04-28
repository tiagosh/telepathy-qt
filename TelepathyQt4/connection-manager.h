/*
 * This file is part of TelepathyQt4
 *
 * Copyright (C) 2008 Collabora Ltd. <http://www.collabora.co.uk/>
 * Copyright (C) 2008 Nokia Corporation
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */

#ifndef _TelepathyQt4_cli_connection_manager_h_HEADER_GUARD_
#define _TelepathyQt4_cli_connection_manager_h_HEADER_GUARD_

#ifndef IN_TELEPATHY_QT4_HEADER
#error IN_TELEPATHY_QT4_HEADER
#endif

#include <TelepathyQt4/_gen/cli-connection-manager.h>

#include <TelepathyQt4/DBus>
#include <TelepathyQt4/DBusProxy>
#include <TelepathyQt4/OptionalInterfaceFactory>
#include <TelepathyQt4/ReadinessHelper>
#include <TelepathyQt4/ReadyObject>
#include <TelepathyQt4/Types>
#include <TelepathyQt4/Constants>
#include <TelepathyQt4/SharedPtr>

#include <QSet>

namespace Tp
{

class PendingConnection;
class PendingReady;
class PendingStringList;
class ProtocolParameter;
class ProtocolInfo;

typedef QList<ProtocolParameter*> ProtocolParameterList;
typedef QList<ProtocolInfo*> ProtocolInfoList;

class ProtocolParameter
{
public:
    ProtocolParameter(const QString &name,
                      const QDBusSignature &dbusSignature,
                      QVariant defaultValue,
                      ConnMgrParamFlag flags);
    ~ProtocolParameter();

    QString name() const { return mName; }
    QDBusSignature dbusSignature() const { return mDBusSignature; }
    QVariant::Type type() const { return mType; }
    QVariant defaultValue() const { return mDefaultValue; }

    bool isRequired() const;
    bool isSecret() const;
    bool requiredForRegistration() const;

    bool operator==(const ProtocolParameter &other) const;
    bool operator==(const QString &name) const;

private:
    Q_DISABLE_COPY(ProtocolParameter);

    struct Private;
    friend struct Private;
    Private *mPriv;
    QString mName;
    QDBusSignature mDBusSignature;
    QVariant::Type mType;
    QVariant mDefaultValue;
    ConnMgrParamFlag mFlags;
};


class ProtocolInfo
{
public:
    ~ProtocolInfo();

    QString cmName() const { return mCmName; }

    QString name() const { return mName; }

    const ProtocolParameterList &parameters() const;
    bool hasParameter(const QString &name) const;

    bool canRegister() const;

private:
    Q_DISABLE_COPY(ProtocolInfo);

    ProtocolInfo(const QString &cmName, const QString &name);

    void addParameter(const ParamSpec &spec);

    class Private;
    friend class Private;
    friend class ConnectionManager;
    Private *mPriv;
    QString mCmName;
    QString mName;
};


class ConnectionManager : public StatelessDBusProxy,
                          private OptionalInterfaceFactory<ConnectionManager>,
                          public ReadyObject,
                          public RefCounted
{
    Q_OBJECT

public:
    static const Feature FeatureCore;

    static ConnectionManagerPtr create(const QString &name);
    static ConnectionManagerPtr create(const QDBusConnection &bus,
            const QString &name);

    virtual ~ConnectionManager();

    QString name() const;

    QStringList interfaces() const;

    QStringList supportedProtocols() const;
    const ProtocolInfoList &protocols() const;

    PendingConnection *requestConnection(const QString &protocol,
            const QVariantMap &parameters);

    inline Client::DBus::PropertiesInterface *propertiesInterface() const
    {
        return OptionalInterfaceFactory<ConnectionManager>::interface<Client::DBus::PropertiesInterface>();
    }

    static PendingStringList *listNames(const QDBusConnection &bus = QDBusConnection::sessionBus());

protected:
    ConnectionManager(const QString &name);
    ConnectionManager(const QDBusConnection &bus, const QString &name);

    Client::ConnectionManagerInterface *baseInterface() const;

private Q_SLOTS:
    void gotMainProperties(QDBusPendingCallWatcher *);
    void gotProtocols(QDBusPendingCallWatcher *);
    void gotParameters(QDBusPendingCallWatcher *);

private:
    Q_DISABLE_COPY(ConnectionManager);

    struct Private;
    friend struct Private;
    friend class PendingConnection;
    Private *mPriv;
};

} // Tp

#endif