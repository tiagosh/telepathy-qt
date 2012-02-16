/**
 * This file is part of TelepathyQt
 *
 * @copyright Copyright (C) 2012 Collabora Ltd. <http://www.collabora.co.uk/>
 * @copyright Copyright (C) 2012 Nokia Corporation
 * @license LGPL 2.1
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

#include <TelepathyQt/Service/BaseConnectionManager>
#include "TelepathyQt/Service/base-connection-manager-internal.h"

#include "TelepathyQt/Service/_gen/base-connection-manager-internal.moc.hpp"

#include "TelepathyQt/debug-internal.h"

#include <TelepathyQt/Constants>

#include <QDBusObjectPath>
#include <QString>
#include <QStringList>

namespace Tp
{
namespace Service
{

struct TP_QT_SVC_NO_EXPORT BaseConnectionManager::Private
{
    Private(BaseConnectionManager *parent, const QDBusConnection &dbusConnection,
            const QString &cmName)
        : parent(parent),
          dbusConnection(dbusConnection),
          cmName(cmName),
          adaptee(new BaseConnectionManager::Adaptee(dbusConnection, parent)),
          registered(false)
    {
    }

    BaseConnectionManager *parent;
    QDBusConnection dbusConnection;
    QString cmName;

    BaseConnectionManager::Adaptee *adaptee;
    bool registered;
};

BaseConnectionManager::Adaptee::Adaptee(const QDBusConnection &dbusConnection,
        BaseConnectionManager *baseCM)
    : QObject(),
      mBaseCM(baseCM)
{
    mAdaptor = new ConnectionManagerAdaptor(dbusConnection, this, this);
}

BaseConnectionManager::Adaptee::~Adaptee()
{
}

QStringList BaseConnectionManager::Adaptee::interfaces() const
{
    return QStringList() << QString(QLatin1String("ofdT.Test"));
}

ProtocolPropertiesMap BaseConnectionManager::Adaptee::protocols() const
{
    return ProtocolPropertiesMap();
}

void BaseConnectionManager::Adaptee::getParameters(const QString &protocol,
        const ConnectionManagerAdaptor::GetParametersContextPtr &context)
{
    qDebug() << __FUNCTION__ << "called";
    context->setFinished(ParamSpecList());
}

void BaseConnectionManager::Adaptee::listProtocols(
        const ConnectionManagerAdaptor::ListProtocolsContextPtr &context)
{
    qDebug() << __FUNCTION__ << "called";
    context->setFinished(QStringList());
}

void BaseConnectionManager::Adaptee::requestConnection(const QString &protocol,
        const QVariantMap &params,
        const ConnectionManagerAdaptor::RequestConnectionContextPtr &context)
{
    // emit newConnection()
    qDebug() << __FUNCTION__ << "called";
    context->setFinished(QString(), QDBusObjectPath(QLatin1String("/")));
}

BaseConnectionManager::BaseConnectionManager(const QDBusConnection &dbusConnection, const QString &cmName)
    : mPriv(new Private(this, dbusConnection, cmName))
{
}

BaseConnectionManager::~BaseConnectionManager()
{
    delete mPriv;
}

QDBusConnection BaseConnectionManager::dbusConnection() const
{
    return mPriv->dbusConnection;
}

bool BaseConnectionManager::registerObject()
{
    if (mPriv->registered) {
        debug() << "Connection manager already registered";
        return true;
    }

    QString busName = TP_QT_CONNECTION_MANAGER_BUS_NAME_BASE;
    busName.append(mPriv->cmName);
    if (!mPriv->dbusConnection.registerService(busName)) {
        warning() << "Unable to register connection manager: busName" <<
            busName << "already registered";
        return false;
    }

    QString objectPath = TP_QT_CONNECTION_MANAGER_OBJECT_PATH_BASE;
    objectPath.append(mPriv->cmName);
    if (!mPriv->dbusConnection.registerObject(objectPath, mPriv->adaptee)) {
        // this shouldn't happen, but let's make sure
        warning() << "Unable to register connection manager: objectPath" <<
            objectPath << "already registered";
        return false;
    }

    debug() << "Connection manager registered - busName:" << busName <<
        "objectPath:" << objectPath;

    mPriv->registered = true;
    return true;
}

}
}
