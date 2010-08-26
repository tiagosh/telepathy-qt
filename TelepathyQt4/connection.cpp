/*
 * This file is part of TelepathyQt4
 *
 * Copyright (C) 2008, 2009 Collabora Ltd. <http://www.collabora.co.uk/>
 * Copyright (C) 2008, 2009 Nokia Corporation
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

#include <TelepathyQt4/Connection>
#include "TelepathyQt4/connection-internal.h"

#include "TelepathyQt4/_gen/cli-connection.moc.hpp"
#include "TelepathyQt4/_gen/cli-connection-body.hpp"
#include "TelepathyQt4/_gen/connection.moc.hpp"
#include "TelepathyQt4/_gen/connection-internal.moc.hpp"

#include "TelepathyQt4/debug-internal.h"

#include <TelepathyQt4/ConnectionCapabilities>
#include <TelepathyQt4/ContactManager>
#include <TelepathyQt4/PendingChannel>
#include <TelepathyQt4/PendingContactAttributes>
#include <TelepathyQt4/PendingContacts>
#include <TelepathyQt4/PendingFailure>
#include <TelepathyQt4/PendingHandles>
#include <TelepathyQt4/PendingReady>
#include <TelepathyQt4/PendingVoid>
#include <TelepathyQt4/ReferencedHandles>

#include <QMap>
#include <QMetaObject>
#include <QMutex>
#include <QMutexLocker>
#include <QPair>
#include <QQueue>
#include <QString>
#include <QTimer>
#include <QtGlobal>

namespace Tp
{

struct TELEPATHY_QT4_NO_EXPORT Connection::Private
{
    Private(Connection *parent);
    ~Private();

    void init();

    static void introspectMain(Private *self);
    void introspectMainProperties();
    void introspectMainFallbackStatus();
    void introspectMainFallbackInterfaces();
    void introspectMainFallbackSelfHandle();
    void introspectCapabilities();
    void introspectContactAttributeInterfaces();
    static void introspectSelfContact(Private *self);
    static void introspectSimplePresence(Private *self);
    static void introspectRoster(Private *self);
    static void introspectRosterGroups(Private *self);
    static void introspectBalance(Private *self);

    void continueMainIntrospection();
    void setCurrentStatus(uint status);
    void forceCurrentStatus(uint status);
    void setInterfaces(const QStringList &interfaces);

    // Should always be used instead of directly using baseclass invalidate()
    void invalidateResetCaps(const QString &errorName, const QString &errorMessage);

    void checkFeatureRosterGroupsReady();

    struct HandleContext;

    // Public object
    Connection *parent;

    // Instance of generated interface class
    Client::ConnectionInterface *baseInterface;

    // Mandatory properties interface proxy
    Client::DBus::PropertiesInterface *properties;

    // Optional interface proxies
    Client::ConnectionInterfaceSimplePresenceInterface *simplePresence;

    ReadinessHelper *readinessHelper;

    // Introspection
    QQueue<void (Private::*)()> introspectMainQueue;

    // FeatureCore
    // keep pendingStatus and pendingStatusReason until we emit statusChanged
    // so Connection::status() and Connection::statusReason() are consistent
    bool introspectingMain;
    bool statusChangedWhileIntrospectingMain;

    uint pendingStatus;
    uint pendingStatusReason;
    uint status;
    uint statusReason;
    ErrorDetails errorDetails;

    uint selfHandle;

    ConnectionCapabilities *caps;

    ContactManager *contactManager;

    // FeatureSelfContact
    ContactPtr selfContact;
    QStringList contactAttributeInterfaces;

    // FeatureSimplePresence
    SimpleStatusSpecMap simplePresenceStatuses;

    // FeatureRoster
    QMap<uint, ContactManager::ContactListChannel> contactListChannels;
    uint contactListChannelsReady;

    // FeatureRosterGroups
    QList<ChannelPtr> contactListGroupChannels;
    // Number of things left to do before the Groups feature is ready
    // 1 for Get("Channels") + 1 per channel not ready
    uint featureRosterGroupsTodo;

    // FeatureAccountBalance
    CurrencyAmount accountBalance;

    // misc
    // (Bus connection name, service name) -> HandleContext
    static QMap<QPair<QString, QString>, HandleContext *> handleContexts;
    static QMutex handleContextsLock;
    HandleContext *handleContext;
};

// Handle tracking
struct TELEPATHY_QT4_NO_EXPORT Connection::Private::HandleContext
{
    struct Type
    {
        QMap<uint, uint> refcounts;
        QSet<uint> toRelease;
        uint requestsInFlight;
        bool releaseScheduled;

        Type()
            : requestsInFlight(0),
              releaseScheduled(false)
        {
        }
    };

    HandleContext()
        : refcount(0)
    {
    }

    int refcount;
    QMutex lock;
    QMap<uint, Type> types;
};

Connection::Private::Private(Connection *parent)
    : parent(parent),
      baseInterface(new Client::ConnectionInterface(parent->dbusConnection(),
                    parent->busName(), parent->objectPath(), parent)),
      properties(parent->propertiesInterface()),
      simplePresence(0),
      readinessHelper(parent->readinessHelper()),
      introspectingMain(false),
      statusChangedWhileIntrospectingMain(false),
      pendingStatus(Connection::StatusUnknown),
      pendingStatusReason(ConnectionStatusReasonNoneSpecified),
      status(Connection::StatusUnknown),
      statusReason(ConnectionStatusReasonNoneSpecified),
      selfHandle(0),
      caps(new ConnectionCapabilities()),
      contactManager(new ContactManager(parent)),
      contactListChannelsReady(0),
      featureRosterGroupsTodo(0),
      handleContext(0)
{
    Q_ASSERT(properties != 0);

    init();

    ReadinessHelper::Introspectables introspectables;

    ReadinessHelper::Introspectable introspectableCore(
        QSet<uint>() << Connection::StatusUnknown <<
                        Connection::StatusDisconnected <<
                        Connection::StatusConnected,                                   // makesSenseForStatuses
        Features(),                                                                    // dependsOnFeatures (none)
        QStringList(),                                                                 // dependsOnInterfaces (none)
        (ReadinessHelper::IntrospectFunc) &Private::introspectMain,
        this);
    introspectables[FeatureCore] = introspectableCore;

    ReadinessHelper::Introspectable introspectableSelfContact(
        QSet<uint>() << Connection::StatusConnected,                                   // makesSenseForStatuses
        Features() << FeatureCore,                                                     // dependsOnFeatures (core)
        QStringList(),                                                                 // dependsOnInterfaces
        (ReadinessHelper::IntrospectFunc) &Private::introspectSelfContact,
        this);
    introspectables[FeatureSelfContact] = introspectableSelfContact;

    ReadinessHelper::Introspectable introspectableSimplePresence(
        QSet<uint>() << Connection::StatusConnected,                                                // makesSenseForStatuses
        Features() << FeatureCore,                                                                  // dependsOnFeatures (core)
        QStringList() << QLatin1String(TELEPATHY_INTERFACE_CONNECTION_INTERFACE_SIMPLE_PRESENCE),   // dependsOnInterfaces
        (ReadinessHelper::IntrospectFunc) &Private::introspectSimplePresence,
        this);
    introspectables[FeatureSimplePresence] = introspectableSimplePresence;

    ReadinessHelper::Introspectable introspectableRoster(
        QSet<uint>() << Connection::StatusConnected,                                                // makesSenseForStatuses
        Features() << FeatureCore,                                                                  // dependsOnFeatures (core)
        QStringList() << QLatin1String(TELEPATHY_INTERFACE_CONNECTION_INTERFACE_CONTACTS),          // dependsOnInterfaces
        (ReadinessHelper::IntrospectFunc) &Private::introspectRoster,
        this);
    introspectables[FeatureRoster] = introspectableRoster;

    ReadinessHelper::Introspectable introspectableRosterGroups(
        QSet<uint>() << Connection::StatusConnected,                                                // makesSenseForStatuses
        Features() << FeatureRoster,                                                                // dependsOnFeatures (core)
        QStringList() << QLatin1String(TELEPATHY_INTERFACE_CONNECTION_INTERFACE_REQUESTS),          // dependsOnInterfaces
        (ReadinessHelper::IntrospectFunc) &Private::introspectRosterGroups,
        this);
    introspectables[FeatureRosterGroups] = introspectableRosterGroups;

    ReadinessHelper::Introspectable introspectableBalance(
        QSet<uint>() << Connection::StatusConnected,                                                // makesSenseForStatuses
        Features() << FeatureCore,                                                                  // dependsOnFeatures (core)
        QStringList() << QLatin1String(TELEPATHY_INTERFACE_CONNECTION_INTERFACE_BALANCE),           // dependsOnInterfaces
        (ReadinessHelper::IntrospectFunc) &Private::introspectBalance,
        this);
    introspectables[FeatureAccountBalance] = introspectableBalance;

    readinessHelper->addIntrospectables(introspectables);
    readinessHelper->setCurrentStatus(status);
    parent->connect(readinessHelper,
            SIGNAL(statusReady(uint)),
            SLOT(onStatusReady(uint)));
    readinessHelper->becomeReady(Features() << FeatureCore);
}

Connection::Private::~Private()
{
    // Clear selfContact so its handle will be released cleanly before the
    // handleContext
    selfContact.clear();

    delete caps;

    QMutexLocker locker(&handleContextsLock);

    // All handle contexts locked, so safe
    if (!--handleContext->refcount) {
        debug() << "Destroying HandleContext";

        foreach (uint handleType, handleContext->types.keys()) {
            HandleContext::Type type = handleContext->types[handleType];

            if (!type.refcounts.empty()) {
                debug() << " Still had references to" <<
                    type.refcounts.size() << "handles, releasing now";
                baseInterface->ReleaseHandles(handleType, type.refcounts.keys());
            }

            if (!type.toRelease.empty()) {
                debug() << " Was going to release" <<
                    type.toRelease.size() << "handles, doing that now";
                baseInterface->ReleaseHandles(handleType, type.toRelease.toList());
            }
        }

        handleContexts.remove(qMakePair(baseInterface->connection().name(),
                    baseInterface->service()));
        delete handleContext;
    } else {
        Q_ASSERT(handleContext->refcount > 0);
    }
}

void Connection::Private::init()
{
    debug() << "Connecting to ConnectionError()";
    parent->connect(baseInterface,
            SIGNAL(ConnectionError(const QString &, const QVariantMap &)),
            SLOT(onConnectionError(const QString &, const QVariantMap &)));
    debug() << "Connecting to StatusChanged()";
    parent->connect(baseInterface,
            SIGNAL(StatusChanged(uint, uint)),
            SLOT(onStatusChanged(uint, uint)));
    debug() << "Connecting to SelfHandleChanged()";
    parent->connect(baseInterface,
            SIGNAL(SelfHandleChanged(uint)),
            SLOT(onSelfHandleChanged(uint)));

    QMutexLocker locker(&handleContextsLock);
    QString busConnectionName = baseInterface->connection().name();
    QString busName = baseInterface->service();

    if (handleContexts.contains(qMakePair(busConnectionName, busName))) {
        debug() << "Reusing existing HandleContext";
        handleContext = handleContexts[
            qMakePair(busConnectionName, busName)];
    } else {
        debug() << "Creating new HandleContext";
        handleContext = new HandleContext;
        handleContexts[
            qMakePair(busConnectionName, busName)] = handleContext;
    }

    // All handle contexts locked, so safe
    ++handleContext->refcount;
}

void Connection::Private::introspectMain(Connection::Private *self)
{
    self->introspectingMain = true;
    self->introspectMainProperties();
}

void Connection::Private::introspectMainProperties()
{
    debug() << "Calling Properties::GetAll(Connection)";
    QDBusPendingCallWatcher *watcher =
        new QDBusPendingCallWatcher(
                properties->GetAll(QLatin1String(TELEPATHY_INTERFACE_CONNECTION)),
                parent);
    parent->connect(watcher,
            SIGNAL(finished(QDBusPendingCallWatcher*)),
            SLOT(gotMainProperties(QDBusPendingCallWatcher*)));
}

void Connection::Private::introspectMainFallbackStatus()
{
    debug() << "Calling GetStatus()";
    QDBusPendingCallWatcher *watcher =
        new QDBusPendingCallWatcher(baseInterface->GetStatus(),
                parent);
    parent->connect(watcher,
            SIGNAL(finished(QDBusPendingCallWatcher *)),
            SLOT(gotStatus(QDBusPendingCallWatcher *)));
}

void Connection::Private::introspectMainFallbackInterfaces()
{
    debug() << "Calling GetInterfaces()";
    QDBusPendingCallWatcher *watcher =
        new QDBusPendingCallWatcher(baseInterface->GetInterfaces(),
                parent);
    parent->connect(watcher,
            SIGNAL(finished(QDBusPendingCallWatcher *)),
            SLOT(gotInterfaces(QDBusPendingCallWatcher *)));
}

void Connection::Private::introspectMainFallbackSelfHandle()
{
    debug() << "Calling GetSelfHandle()";
    QDBusPendingCallWatcher *watcher =
        new QDBusPendingCallWatcher(baseInterface->GetSelfHandle(),
                parent);
    parent->connect(watcher,
            SIGNAL(finished(QDBusPendingCallWatcher *)),
            SLOT(gotSelfHandle(QDBusPendingCallWatcher *)));
}

void Connection::Private::introspectCapabilities()
{
    debug() << "Retrieving capabilities";
    QDBusPendingCallWatcher *watcher = new QDBusPendingCallWatcher(
            properties->Get(
                QLatin1String(TELEPATHY_INTERFACE_CONNECTION_INTERFACE_REQUESTS),
                QLatin1String("RequestableChannelClasses")), parent);
    parent->connect(watcher,
            SIGNAL(finished(QDBusPendingCallWatcher *)),
            SLOT(gotCapabilities(QDBusPendingCallWatcher *)));
}

void Connection::Private::introspectContactAttributeInterfaces()
{
    debug() << "Retrieving contact attribute interfaces";
    QDBusPendingCall call =
        properties->Get(
                QLatin1String(TELEPATHY_INTERFACE_CONNECTION_INTERFACE_CONTACTS),
                QLatin1String("ContactAttributeInterfaces"));
    QDBusPendingCallWatcher *watcher =
        new QDBusPendingCallWatcher(call, parent);
    parent->connect(watcher,
                    SIGNAL(finished(QDBusPendingCallWatcher *)),
                    SLOT(gotContactAttributeInterfaces(QDBusPendingCallWatcher *)));
}

void Connection::Private::introspectSelfContact(Connection::Private *self)
{
    debug() << "Building self contact";
    PendingContacts *contacts = self->contactManager->contactsForHandles(
            UIntList() << self->selfHandle);
    self->parent->connect(contacts,
            SIGNAL(finished(Tp::PendingOperation *)),
            SLOT(gotSelfContact(Tp::PendingOperation *)));
}

void Connection::Private::introspectSimplePresence(Connection::Private *self)
{
    Q_ASSERT(self->properties != 0);

    debug() << "Calling Properties::Get("
        "Connection.I.SimplePresence.Statuses)";
    QDBusPendingCall call =
        self->properties->Get(
                QLatin1String(TELEPATHY_INTERFACE_CONNECTION_INTERFACE_SIMPLE_PRESENCE),
                QLatin1String("Statuses"));
    QDBusPendingCallWatcher *watcher =
        new QDBusPendingCallWatcher(call, self->parent);
    self->parent->connect(watcher,
            SIGNAL(finished(QDBusPendingCallWatcher *)),
            SLOT(gotSimpleStatuses(QDBusPendingCallWatcher *)));
}

void Connection::Private::introspectRoster(Connection::Private *self)
{
    debug() << "Requesting handles for contact lists";

    for (uint i = 0; i < ContactManager::ContactListChannel::LastType; ++i) {
        self->contactListChannels.insert(i,
                ContactManager::ContactListChannel(
                    (ContactManager::ContactListChannel::Type) i));

        PendingHandles *pending = self->parent->requestHandles(
                HandleTypeList,
                QStringList() << ContactManager::ContactListChannel::identifierForType(
                    (ContactManager::ContactListChannel::Type) i));
        self->parent->connect(pending,
                SIGNAL(finished(Tp::PendingOperation*)),
                SLOT(gotContactListsHandles(Tp::PendingOperation*)));
    }
}

void Connection::Private::introspectRosterGroups(Connection::Private *self)
{
    debug() << "Introspecting roster groups";

    ++self->featureRosterGroupsTodo; // decremented in gotChannels

    // we already checked if requests interface exists, so bypass requests
    // interface checking
    Client::ConnectionInterfaceRequestsInterface *iface =
        self->parent->requestsInterface(BypassInterfaceCheck);

    debug() << "Connecting to Requests.NewChannels";
    self->parent->connect(iface,
            SIGNAL(NewChannels(const Tp::ChannelDetailsList&)),
            SLOT(onNewChannels(const Tp::ChannelDetailsList&)));

    debug() << "Retrieving channels";
    QDBusPendingCallWatcher *watcher = new QDBusPendingCallWatcher(
            self->parent->propertiesInterface()->Get(
                QLatin1String(TELEPATHY_INTERFACE_CONNECTION_INTERFACE_REQUESTS),
                QLatin1String("Channels")), self->parent);
    self->parent->connect(watcher,
            SIGNAL(finished(QDBusPendingCallWatcher*)),
            SLOT(gotChannels(QDBusPendingCallWatcher*)));
}

void Connection::Private::introspectBalance(Connection::Private *self)
{
    debug() << "Introspecting balance";

    // we already checked if balance interface exists, so bypass requests
    // interface checking
    Client::ConnectionInterfaceBalanceInterface *iface =
        self->parent->balanceInterface(BypassInterfaceCheck);

    debug() << "Connecting to Balance.BalanceChanged";
    self->parent->connect(iface,
            SIGNAL(BalanceChanged(const Tp::CurrencyAmount&)),
            SLOT(onBalanceChanged(const Tp::CurrencyAmount&)));

    debug() << "Retrieving balance";
    QDBusPendingCallWatcher *watcher = new QDBusPendingCallWatcher(
            self->parent->propertiesInterface()->Get(
                QLatin1String(TELEPATHY_INTERFACE_CONNECTION_INTERFACE_BALANCE),
                QLatin1String("AccountBalance")), self->parent);
    self->parent->connect(watcher,
            SIGNAL(finished(QDBusPendingCallWatcher*)),
            SLOT(gotBalance(QDBusPendingCallWatcher*)));
}

void Connection::Private::continueMainIntrospection()
{
    if (introspectMainQueue.isEmpty()) {
        readinessHelper->setIntrospectCompleted(FeatureCore, true);

        introspectingMain = false;

        if (statusChangedWhileIntrospectingMain) {
            statusChangedWhileIntrospectingMain = false;
            readinessHelper->setCurrentStatus(pendingStatus);
        }
    } else {
        (this->*(introspectMainQueue.dequeue()))();
    }
}

void Connection::Private::setCurrentStatus(uint status)
{
    // if the initial introspection is still running, only clear main
    // introspection queue and wait for the last call to return, to avoid
    // the return of the last call wrongly setting FeatureCore as ready for the
    // new status, otherwise set the readinessHelper status to the new status,
    // so it can re-run the introspection if needed.
    if (introspectingMain) {
        introspectMainQueue.clear();
    } else {
        readinessHelper->setCurrentStatus(status);
    }
}

void Connection::Private::forceCurrentStatus(uint status)
{
    // only update the status if we did not get it from StatusChanged
    if (pendingStatus == Connection::StatusUnknown) {
        debug() << "Got status:" << status;
        pendingStatus = status;
        // No need to re-run introspection as we just received the status. Let
        // the introspection continue normally but update readinessHelper with
        // the correct status.
        readinessHelper->forceCurrentStatus(status);
    }
}

void Connection::Private::setInterfaces(const QStringList &interfaces)
{
    debug() << "Got interfaces:" << interfaces;
    parent->setInterfaces(interfaces);
    readinessHelper->setInterfaces(interfaces);
}

void Connection::Private::invalidateResetCaps(const QString &error, const QString &message)
{
    caps->updateRequestableChannelClasses(RequestableChannelClassList());
    parent->invalidate(error, message);
}

void Connection::Private::checkFeatureRosterGroupsReady()
{
    if (featureRosterGroupsTodo != 0) {
        return;
    }

    debug() << "FeatureRosterGroups ready";
    contactManager->setContactListGroupChannels(
            contactListGroupChannels);
    readinessHelper->setIntrospectCompleted(
            FeatureRosterGroups, true);
    contactListGroupChannels.clear();
}

Connection::PendingConnect::PendingConnect(Connection *parent, const Features &requestedFeatures)
    : PendingReady(requestedFeatures, parent, parent)
{
    QDBusPendingCall call = parent->baseInterface()->Connect();
    QDBusPendingCallWatcher *watcher = new QDBusPendingCallWatcher(call, parent);
    connect(watcher,
            SIGNAL(finished(QDBusPendingCallWatcher*)),
            this,
            SLOT(onConnectReply(QDBusPendingCallWatcher*)));
}

void Connection::PendingConnect::onConnectReply(QDBusPendingCallWatcher *watcher)
{
    if (watcher->isError()) {
        setFinishedWithError(watcher->error());
    }
    else {
        connect(qobject_cast<Connection*>(parent())->becomeReady(requestedFeatures()),
                SIGNAL(finished(Tp::PendingOperation*)),
                SLOT(onBecomeReadyReply(Tp::PendingOperation*)));
    }
}

void Connection::PendingConnect::onBecomeReadyReply(Tp::PendingOperation *op)
{
    if (op->isError()) {
        setFinishedWithError(op->errorName(), op->errorMessage());
    }
    else {
        setFinished();
    }
}

QMap<QPair<QString, QString>, Connection::Private::HandleContext*> Connection::Private::handleContexts;
QMutex Connection::Private::handleContextsLock;

/**
 * \class Connection
 * \ingroup clientconn
 * \headerfile TelepathyQt4/connection.h <TelepathyQt4/Connection>
 *
 * \brief The Connection class provides an object representing a Telepathy
 * connection.
 *
 * Connection adds the following features compared to using
 * Client::ConnectionInterface directly:
 * <ul>
 *  <li>Status tracking</li>
 *  <li>Getting the list of supported interfaces automatically</li>
 *  <li>Getting the valid presence statuses automatically</li>
 * </ul>
 *
 * This models a connection to a single user account on a communication service.
 * Its basic capability is to provide the facility to request and receive
 * channels of differing types (such as text channels or streaming media
 * channels) which are used to carry out further communication.
 *
 * Contacts, and server-stored lists (such as subscribed contacts,
 * block lists, or allow lists) on a service are all represented using the
 * ContactManager object on the connection, which is valid only for the lifetime
 * of the connection object.
 *
 * The remote object state accessor functions on this object (status(),
 * statusReason(), and so on) don't make any DBus calls; instead,
 * they return values cached from a previous introspection run. The
 * introspection process populates their values in the most efficient way
 * possible based on what the service implements.
 * Their return value is mostly undefined until the
 * introspection process is completed, i.e. isReady() returns true. See the
 * individual accessor descriptions for more details. A status change to
 * StatusConnected indicates that the introspection process is finished
 *
 * Signals are emitted to indicate that properties have changed for example
 * statusChanged()(), selfContactChanged(), etc.
 *
 * \section conn_usage_sec Usage
 *
 * \subsection conn_create_sec Creating a connection object
 *
 * The easiest way to create connection objects is through Account. One can
 * just use the Account::connection method to get an account active connection.
 *
 * If you already know the object path, you can just call create().
 * For example:
 *
 * \code ConnectionPtr conn = Connection::create(busName, objectPath); \endcode
 *
 * A ConnectionPtr object is returned, which will automatically keep
 * track of object lifetime.
 *
 * You can also provide a D-Bus connection as a QDBusConnection:
 *
 * \code
 *
 * ConnectionPtr conn = Connection::create(QDBusConnection::sessionBus(),
 *         busName, objectPath);
 *
 * \endcode
 *
 * \subsection conn_ready_sec Making connection ready to use
 *
 * A Connection object needs to become ready before usage, meaning that the
 * introspection process finished and the object accessors can be used.
 *
 * To make the object ready, use becomeReady() and wait for the
 * PendingOperation::finished() signal to be emitted.
 *
 * \code
 *
 * class MyClass : public QObject
 * {
 *     QOBJECT
 *
 * public:
 *     MyClass(QObject *parent = 0);
 *     ~MyClass() { }
 *
 * private Q_SLOTS:
 *     void onConnectionReady(Tp::PendingOperation*);
 *
 * private:
 *     ConnectionPtr conn;
 * };
 *
 * MyClass::MyClass(const QString &busName, const QString &objectPath,
 *         QObject *parent)
 *     : QObject(parent)
 *       conn(Connection::create(busName, objectPath))
 * {
 *     // connect and become ready
 *     connect(conn->requestConnect(),
 *             SIGNAL(finished(Tp::PendingOperation*)),
 *             SLOT(onConnectionReady(Tp::PendingOperation*)));
 * }
 *
 * void MyClass::onConnectionReady(Tp::PendingOperation *op)
 * {
 *     if (op->isError()) {
 *         qWarning() << "Account cannot become ready:" <<
 *             op->errorName() << "-" << op->errorMessage();
 *         return;
 *     }
 *
 *     // Connection is now ready
 * }
 *
 * \endcode
 *
 * See \ref async_model, \ref shared_ptr
 */

/**
 * Feature representing the core that needs to become ready to make the
 * Connection object usable.
 *
 * Note that this feature must be enabled in order to use most Connection
 * methods.
 * See specific methods documentation for more details.
 *
 * When calling isReady(), becomeReady(), this feature is implicitly added
 * to the requested features.
 */
const Feature Connection::FeatureCore = Feature(QLatin1String(Connection::staticMetaObject.className()), 0, true);

/**
 * Feature used to retrieve the connection self contact.
 *
 * See self contact specific methods' documentation for more details.
 */
const Feature Connection::FeatureSelfContact = Feature(QLatin1String(Connection::staticMetaObject.className()), 1);

/**
 * Feature used to retrieve/keep track of the connection self presence.
 *
 * See simple presence specific methods' documentation for more details.
 */
const Feature Connection::FeatureSimplePresence = Feature(QLatin1String(Connection::staticMetaObject.className()), 2);

/**
 * Feature used to enable roster support on Connection::contactManager.
 *
 * See ContactManager roster specific methods' documentation for more details.
 */
const Feature Connection::FeatureRoster = Feature(QLatin1String(Connection::staticMetaObject.className()), 4);

/**
 * Feature used to enable roster groups support on Connection::contactManager.
 *
 * See ContactManager roster groups specific methods' documentation for more
 * details.
 */
const Feature Connection::FeatureRosterGroups = Feature(QLatin1String(Connection::staticMetaObject.className()), 5);

/**
 * Feature used to retrieve/keep track of the connection account balance.
 *
 * See account balance specific methods' documentation for more details.
 */
const Feature Connection::FeatureAccountBalance = Feature(QLatin1String(Connection::staticMetaObject.className()), 6);

/**
 * Create a new connection object using QDBusConnection::sessionBus().
 *
 * \param busName The connection well-known bus name (sometimes called a
 *                "service name").
 * \param objectPath The connection object path.
 * \return A ConnectionPtr pointing to the newly created Connection.
 */
ConnectionPtr Connection::create(const QString &busName,
        const QString &objectPath)
{
    return ConnectionPtr(new Connection(busName, objectPath));
}

/**
 * Create a new connection object using the given \a bus.
 *
 * \param bus QDBusConnection to use.
 * \param busName The connection well-known bus name (sometimes called a
 *                "service name").
 * \param objectPath The connection object path.
 * \return A ConnectionPtr pointing to the newly created Connection.
 */
ConnectionPtr Connection::create(const QDBusConnection &bus,
        const QString &busName, const QString &objectPath)
{
    return ConnectionPtr(new Connection(bus, busName, objectPath));
}

/**
 * Construct a new connection object using QDBusConnection::sessionBus().
 *
 * \param busName The connection's well-known bus name (sometimes called a
 *                "service name").
 * \param objectPath The connection object path.
 */
Connection::Connection(const QString &busName,
                       const QString &objectPath)
    : StatefulDBusProxy(QDBusConnection::sessionBus(),
            busName, objectPath),
      OptionalInterfaceFactory<Connection>(this),
      ReadyObject(this, FeatureCore),
      mPriv(new Private(this))
{
}

/**
 * Construct a new connection object using the given \bus.
 *
 * \param bus QDBusConnection to use.
 * \param busName The connection's well-known bus name (sometimes called a
 *                "service name").
 * \param objectPath The connection object path.
 */
Connection::Connection(const QDBusConnection &bus,
                       const QString &busName,
                       const QString &objectPath)
    : StatefulDBusProxy(bus, busName, objectPath),
      OptionalInterfaceFactory<Connection>(this),
      ReadyObject(this, FeatureCore),
      mPriv(new Private(this))
{
}

/**
 * Class destructor.
 */
Connection::~Connection()
{
    delete mPriv;
}

/**
 * Return the status of this connection.
 *
 * This method requires Connection::FeatureCore to be enabled.
 *
 * \return The status of this connection, as defined in Connection::Status.
 * \sa statusChanged()
 */
Connection::Status Connection::status() const
{
    return (Connection::Status) mPriv->status;
}

/**
 * Return the reason for this connection's status (which is returned by
 * status()). The validity and change rules are the same as for status().
 *
 * This method requires Connection::FeatureCore to be enabled.
 *
 * \return The reason, as defined in ConnectionStatusReason.
 */
ConnectionStatusReason Connection::statusReason() const
{
    return (ConnectionStatusReason) mPriv->statusReason;
}

struct Connection::ErrorDetails::Private : public QSharedData
{
    Private(const QVariantMap &details)
        : details(details) {}

    QVariantMap details;
};

Connection::ErrorDetails::ErrorDetails()
    : mPriv(0)
{
}

Connection::ErrorDetails::ErrorDetails(const ErrorDetails &other)
    : mPriv(other.mPriv)
{
}

Connection::ErrorDetails::~ErrorDetails()
{
}

Connection::ErrorDetails &Connection::ErrorDetails::operator=(
        const ErrorDetails &other)
{
    this->mPriv = other.mPriv;
    return *this;
}

QVariantMap Connection::ErrorDetails::allDetails() const
{
    return isValid() ? mPriv->details : QVariantMap();
}

Connection::ErrorDetails::ErrorDetails(const QVariantMap &details)
    : mPriv(new Private(details))
{
}

const Connection::ErrorDetails &Connection::errorDetails() const
{
    if (isValid())
        warning() << "Connection::errorDetails() used on" << objectPath() << "which is valid";

    return mPriv->errorDetails;
}

/**
 * Return the handle which represents the user on this connection, which will
 * remain valid for the lifetime of this connection, or until a change in the
 * user's identifier is signalled by the selfHandleChanged() signal. If the
 * connection is not yet in the StatusConnected state, the value of this
 * property may be zero.
 *
 * This method requires Connection::FeatureCore to be enabled.
 *
 * \return Self handle.
 */
uint Connection::selfHandle() const
{
    return mPriv->selfHandle;
}

/**
 * Return a dictionary of presence statuses valid for use in this connection.
 *
 * The value may have changed arbitrarily during the time the
 * Connection spends in status StatusConnecting,
 * again staying fixed for the entire time in StatusConnected.
 *
 * This method requires Connection::FeatureSimplePresence to be enabled.
 *
 * \return Dictionary from string identifiers to structs for each valid
 * status.
 */
SimpleStatusSpecMap Connection::allowedPresenceStatuses() const
{
    if (!isReady(Features() << FeatureSimplePresence)) {
        warning() << "Trying to retrieve simple presence from connection, but "
                     "simple presence is not supported or was not requested. "
                     "Use becomeReady(FeatureSimplePresence)";
    }

    return mPriv->simplePresenceStatuses;
}

/**
 * Set the self presence status.
 *
 * \a status must be one of the allowed statuses returned by
 * allowedPresenceStatuses().
 *
 * Note that clients SHOULD set the status message for the local user to the
 * empty string, unless the user has actually provided a specific message (i.e.
 * one that conveys more information than the Status).
 *
 * \param status The desired status.
 * \param statusMessage The desired status message.
 * \return A PendingOperation which will emit PendingOperation::finished
 *         when the call has finished.
 *
 * \sa allowedPresenceStatuses()
 */
PendingOperation *Connection::setSelfPresence(const QString &status,
        const QString &statusMessage)
{
    if (!interfaces().contains(QLatin1String(TELEPATHY_INTERFACE_CONNECTION_INTERFACE_SIMPLE_PRESENCE))) {
        return new PendingFailure(QLatin1String(TELEPATHY_ERROR_NOT_IMPLEMENTED),
                QLatin1String("Connection does not support SimplePresence"), this);
    }
    return new PendingVoid(
            simplePresenceInterface()->SetPresence(status, statusMessage),
            this);
}

/**
 * Return the object that represents the contact of this connection.
 *
 * This method requires Connection::FeatureSelfContact to be enabled.
 *
 * \return The connection self contact.
 */
ContactPtr Connection::selfContact() const
{
    if (!isReady(FeatureSelfContact)) {
        warning() << "Connection::selfContact() used, but becomeReady(FeatureSelfContact) "
            "hasn't been completed!";
    }

    return mPriv->selfContact;
}

/**
 * Return the user's balance on the account corresponding to this connection. A
 * negative amount may be possible on some services, and indicates that the user
 * owes money to the service provider.
 *
 * \return The user's balance.
 * \sa accountBalanceChanged()
 */
CurrencyAmount Connection::accountBalance() const
{
    if (!isReady(FeatureAccountBalance)) {
        warning() << "Connection::accountBalance() used before connection "
            "FeatureAccountBalance is ready";
    }

    return mPriv->accountBalance;
}

/**
 * Return the capabilities that are expected to be available on this connection,
 * i.e. those for which createChannel() can reasonably be expected to succeed.
 * User interfaces can use this information to show or hide UI components.
 *
 * This property cannot change after the connection has gone to state()
 * %Tp::Connection_Status_Connected, so there is no change notification.
 *
 * This method requires Connection::FeatureCore to be enabled.
 *
 * @return An object representing the connection capabilities or 0 if
 *         FeatureCore is not ready.
 */
ConnectionCapabilities *Connection::capabilities() const
{
    if (!isReady()) {
        warning() << "Connection::capabilities() used before connection "
            "FeatureCore is ready";
    }

    return mPriv->caps;
}

/**
 * \fn Connection::optionalInterface(InterfaceSupportedChecking check) const
 *
 * Get a pointer to a valid instance of a given %Connection optional
 * interface class, associated with the same remote object the Connection is
 * associated with, and destroyed at the same time the Connection is
 * destroyed.
 *
 * If the list returned by interfaces() doesn't contain the name of the
 * interface requested <code>0</code> is returned. This check can be
 * bypassed by specifying #BypassInterfaceCheck for <code>check</code>, in
 * which case a valid instance is always returned.
 *
 * If the object is not ready, the list returned by interfaces() isn't
 * guaranteed to yet represent the full set of interfaces supported by the
 * remote object.
 * Hence the check might fail even if the remote object actually supports
 * the requested interface; using #BypassInterfaceCheck is suggested when
 * the Connection is not suitably ready.
 *
 * \sa OptionalInterfaceFactory::interface
 *
 * \tparam Interface Class of the optional interface to get.
 * \param check Should an instance be returned even if it can't be
 *              determined that the remote object supports the
 *              requested interface.
 * \return Pointer to an instance of the interface class, or <code>0</code>.
 */

/**
 * \fn ConnectionInterfaceAliasingInterface *Connection::aliasingInterface(InterfaceSupportedChecking check) const
 *
 * Convenience function for getting an Aliasing interface proxy.
 *
 * \param check Passed to optionalInterface()
 * \return <code>optionalInterface<ConnectionInterfaceAliasingInterface>(check)</code>
 */

/**
 * \fn ConnectionInterfaceAvatarsInterface *Connection::avatarsInterface(InterfaceSupportedChecking check) const
 *
 * Convenience function for getting an Avatars interface proxy.
 *
 * \param check Passed to optionalInterface()
 * \return <code>optionalInterface<ConnectionInterfaceAvatarsInterface>(check)</code>
 */

/**
 * \fn ConnectionInterfaceCapabilitiesInterface *Connection::capabilitiesInterface(InterfaceSupportedChecking check) const
 *
 * Convenience function for getting a Capabilities interface proxy.
 *
 * \param check Passed to optionalInterface()
 * \return <code>optionalInterface<ConnectionInterfaceCapabilitiesInterface>(check)</code>
 */

/**
 * \fn ConnectionInterfacePresenceInterface *Connection::presenceInterface(InterfaceSupportedChecking check) const
 *
 * Convenience function for getting a Presence interface proxy.
 *
 * \param check Passed to optionalInterface()
 * \return <code>optionalInterface<ConnectionInterfacePresenceInterface>(check)</code>
 */

/**
 * \fn ConnectionInterfaceSimplePresenceInterface *Connection::simplePresenceInterface(InterfaceSupportedChecking check) const
 *
 * Convenience function for getting a SimplePresence interface proxy.
 *
 * \param check Passed to optionalInterface()
 * \return <code>optionalInterface<ConnectionInterfaceSimplePresenceInterface>(check)</code>
 */

/**
 * \fn DBus::PropertiesInterface *Connection::propertiesInterface() const
 *
 * Convenience function for getting a Properties interface proxy. The
 * Properties interface is not necessarily reported by the services, so a
 * <code>check</code> parameter is not provided, and the interface is
 * always assumed to be present.
 *
 * \sa optionalInterface()
 *
 * \return <code>optionalInterface<DBus::PropertiesInterface>(BypassInterfaceCheck)</code>
 */

/**
 * \fn void Connection::accountBalanceChanged(const Tp::CurrencyAmount &accountBalance) const
 *
 * Signal emitted when the user's balance on the account corresponding to this
 * connection changes.
 *
 * \param accountBalance The new user's balance.
 * \sa accountBalance()
 */

void Connection::onStatusReady(uint status)
{
    Q_ASSERT(status == mPriv->pendingStatus);

    mPriv->status = status;
    mPriv->statusReason = mPriv->pendingStatusReason;
    emit statusChanged((Connection::Status) mPriv->status,
            (ConnectionStatusReason) mPriv->statusReason);
}

void Connection::onStatusChanged(uint status, uint reason)
{
    debug() << "StatusChanged from" << mPriv->pendingStatus
            << "to" << status << "with reason" << reason;

    if (mPriv->pendingStatus == status) {
        warning() << "New status was the same as the old status! Ignoring"
            "redundant StatusChanged";
        return;
    }

    if (mPriv->introspectingMain) {
        mPriv->statusChangedWhileIntrospectingMain = true;
    }

    uint oldStatus = mPriv->pendingStatus;
    mPriv->pendingStatus = status;
    mPriv->pendingStatusReason = reason;

    switch (status) {
        case ConnectionStatusConnected:
            debug() << "Performing introspection for the Connected status";
            mPriv->setCurrentStatus(status);
            break;

        case ConnectionStatusConnecting:
            mPriv->setCurrentStatus(status);
            break;

        case ConnectionStatusDisconnected:
            {
                QString errorName = ConnectionHelper::statusReasonToErrorName(
                        (ConnectionStatusReason) reason,
                        (ConnectionStatus) oldStatus);

                // TODO should we signal statusChanged to Disconnected here or just
                //      invalidate?
                //      Also none of the pendingOperations will finish. The
                //      user should just consider them to fail as the connection
                //      is invalid
                onStatusReady(StatusDisconnected);
                mPriv->invalidateResetCaps(errorName,
                        QString(QLatin1String("ConnectionStatusReason = %1")).arg(uint(reason)));
            }
            break;

        default:
            warning() << "Unknown connection status" << status;
            break;
    }
}

void Connection::onConnectionError(const QString &error,
        const QVariantMap &details)
{
    debug().nospace() << "Connection(" << objectPath() << ") got ConnectionError(" << error
        << ") with " << details.size() << " details";

    mPriv->errorDetails = details;
    mPriv->invalidateResetCaps(error,
            details.value(QLatin1String("debug-message")).toString());
}

void Connection::gotMainProperties(QDBusPendingCallWatcher *watcher)
{
    QDBusPendingReply<QVariantMap> reply = *watcher;
    QVariantMap props;

    if (!reply.isError()) {
        props = reply.value();
    } else {
        warning().nospace() << "Properties::GetAll(Connection) failed with " <<
            reply.error().name() << ": " << reply.error().message();
        // let's try to fallback first before failing
    }

    if (props.contains(QLatin1String("Status"))) {
        mPriv->forceCurrentStatus(qdbus_cast<uint>(
                    props[QLatin1String("Status")]));
    } else {
        // only introspect status if we did not got it from StatusChanged
        if (mPriv->pendingStatus == StatusUnknown) {
            mPriv->introspectMainQueue.enqueue(
                    &Private::introspectMainFallbackStatus);
        }
    }

    if (props.contains(QLatin1String("Interfaces"))) {
        mPriv->setInterfaces(qdbus_cast<QStringList>(
                    props[QLatin1String("Interfaces")]));
    } else {
        mPriv->introspectMainQueue.enqueue(
                &Private::introspectMainFallbackInterfaces);
    }

    if (props.contains(QLatin1String("SelfHandle"))) {
        mPriv->selfHandle = qdbus_cast<uint>(
                props[QLatin1String("SelfHandle")]);
    } else {
        mPriv->introspectMainQueue.enqueue(
                &Private::introspectMainFallbackSelfHandle);
    }

    if (hasInterface(TELEPATHY_INTERFACE_CONNECTION_INTERFACE_REQUESTS)) {
        mPriv->introspectMainQueue.enqueue(
                &Private::introspectCapabilities);
    }

    if (hasInterface(TELEPATHY_INTERFACE_CONNECTION_INTERFACE_CONTACTS)) {
        mPriv->introspectMainQueue.enqueue(
                &Private::introspectContactAttributeInterfaces);
    }

    mPriv->continueMainIntrospection();

    watcher->deleteLater();
}

void Connection::gotStatus(QDBusPendingCallWatcher *watcher)
{
    QDBusPendingReply<uint> reply = *watcher;

    if (!reply.isError()) {
        mPriv->forceCurrentStatus(reply.value());

        mPriv->continueMainIntrospection();
    } else {
        warning().nospace() << "GetStatus() failed with " <<
            reply.error().name() << ": " << reply.error().message();
        mPriv->invalidateResetCaps(reply.error().name(), reply.error().message());
    }

    watcher->deleteLater();
}

void Connection::gotInterfaces(QDBusPendingCallWatcher *watcher)
{
    QDBusPendingReply<QStringList> reply = *watcher;

    if (!reply.isError()) {
        mPriv->setInterfaces(reply.value());
    }
    else {
        warning().nospace() << "GetInterfaces() failed with " <<
            reply.error().name() << ": " << reply.error().message() <<
            " - assuming no new interfaces";
        // let's not fail if GetInterfaces fail
    }

    mPriv->continueMainIntrospection();

    watcher->deleteLater();
}

void Connection::gotSelfHandle(QDBusPendingCallWatcher *watcher)
{
    QDBusPendingReply<uint> reply = *watcher;

    if (!reply.isError()) {
        mPriv->selfHandle = reply.value();
        debug() << "Got self handle:" << mPriv->selfHandle;

        mPriv->continueMainIntrospection();
    } else {
        warning().nospace() << "GetSelfHandle() failed with " <<
            reply.error().name() << ": " << reply.error().message();
        mPriv->readinessHelper->setIntrospectCompleted(FeatureCore,
                false, reply.error());
    }

    watcher->deleteLater();
}

void Connection::gotCapabilities(QDBusPendingCallWatcher *watcher)
{
    QDBusPendingReply<QDBusVariant> reply = *watcher;

    if (!reply.isError()) {
        debug() << "Got capabilities";
        mPriv->caps->updateRequestableChannelClasses(
                qdbus_cast<RequestableChannelClassList>(reply.value().variant()));
    } else {
        warning().nospace() << "Getting capabilities failed with " <<
            reply.error().name() << ": " << reply.error().message();
        // let's not fail if retrieving capabilities fail
    }

    mPriv->continueMainIntrospection();

    watcher->deleteLater();
}

void Connection::gotContactAttributeInterfaces(QDBusPendingCallWatcher *watcher)
{
    QDBusPendingReply<QDBusVariant> reply = *watcher;

    if (!reply.isError()) {
        debug() << "Got contact attribute interfaces";
        mPriv->contactAttributeInterfaces = qdbus_cast<QStringList>(reply.value().variant());
    } else {
        warning().nospace() << "Getting contact attribute interfaces failed with " <<
            reply.error().name() << ": " << reply.error().message();
        // let's not fail if retrieving contact attribute interfaces fail
        // TODO should we remove Contacts interface from interfaces?
    }

    mPriv->continueMainIntrospection();

    watcher->deleteLater();
}

void Connection::gotSimpleStatuses(QDBusPendingCallWatcher *watcher)
{
    QDBusPendingReply<QDBusVariant> reply = *watcher;

    if (!reply.isError()) {
        mPriv->simplePresenceStatuses = qdbus_cast<SimpleStatusSpecMap>(reply.value().variant());
        debug() << "Got" << mPriv->simplePresenceStatuses.size() << "simple presence statuses";
        mPriv->readinessHelper->setIntrospectCompleted(FeatureSimplePresence, true);
    }
    else {
        warning().nospace() << "Getting simple presence statuses failed with " <<
            reply.error().name() << ":" << reply.error().message();
        mPriv->readinessHelper->setIntrospectCompleted(FeatureSimplePresence, false, reply.error());
    }

    watcher->deleteLater();
}

void Connection::gotSelfContact(PendingOperation *op)
{
    PendingContacts *pending = qobject_cast<PendingContacts *>(op);

    if (pending->isValid()) {
        Q_ASSERT(pending->contacts().size() == 1);
        ContactPtr contact = pending->contacts()[0];

        if (mPriv->selfContact != contact) {
            mPriv->selfContact = contact;

            // first time
            if (!mPriv->readinessHelper->actualFeatures().contains(FeatureSelfContact)) {
                mPriv->readinessHelper->setIntrospectCompleted(FeatureSelfContact, true);
            }

            emit selfContactChanged();
        }
    } else {
        warning().nospace() << "Getting self contact failed with " <<
            pending->errorName() << ":" << pending->errorMessage();

        // check if the feature is already there, and for some reason introspectSelfContact
        // failed when called the second time
        if (!mPriv->readinessHelper->missingFeatures().contains(FeatureSelfContact)) {
            mPriv->readinessHelper->setIntrospectCompleted(FeatureSelfContact, false,
                    op->errorName(), op->errorMessage());
        }
    }
}


void Connection::gotContactListsHandles(PendingOperation *op)
{
    if (op->isError()) {
        // let's not fail, because the contact lists are not supported
        debug() << "Unable to retrieve contact list handle, ignoring";
        contactListChannelReady();
        return;
    }

    debug() << "Got handles for contact lists";
    PendingHandles *pending = qobject_cast<PendingHandles*>(op);

    if (pending->invalidNames().size() == 1) {
        // let's not fail, because the contact lists are not supported
        debug() << "Unable to retrieve contact list handle, ignoring";
        contactListChannelReady();
        return;
    }

    debug() << "Requesting channels for contact lists";
    QVariantMap request;
    request.insert(QLatin1String(TELEPATHY_INTERFACE_CHANNEL ".ChannelType"),
                   QLatin1String(TELEPATHY_INTERFACE_CHANNEL_TYPE_CONTACT_LIST));
    request.insert(QLatin1String(TELEPATHY_INTERFACE_CHANNEL ".TargetHandleType"),
                   (uint) HandleTypeList);

    Q_ASSERT(pending->handles().size() == 1);
    Q_ASSERT(pending->namesRequested().size() == 1);
    ReferencedHandles handle = pending->handles();
    uint type = ContactManager::ContactListChannel::typeForIdentifier(
            pending->namesRequested().first());
    Q_ASSERT(type != (uint) -1 && type < ContactManager::ContactListChannel::LastType);
    mPriv->contactListChannels[type].handle = handle;
    request[QLatin1String(TELEPATHY_INTERFACE_CHANNEL ".TargetHandle")] = handle[0];
    connect(ensureChannel(request),
            SIGNAL(finished(Tp::PendingOperation*)),
            SLOT(gotContactListChannel(Tp::PendingOperation*)));
}

void Connection::gotContactListChannel(PendingOperation *op)
{
    if (op->isError()) {
        contactListChannelReady();
        return;
    }

    PendingChannel *pending = qobject_cast<PendingChannel*>(op);
    ChannelPtr channel = pending->channel();
    uint handle = pending->targetHandle();
    Q_ASSERT(channel);
    Q_ASSERT(handle);
    for (int i = 0; i < ContactManager::ContactListChannel::LastType; ++i) {
        if (mPriv->contactListChannels[i].handle.size() > 0 &&
            mPriv->contactListChannels[i].handle[0] == handle) {
            Q_ASSERT(!mPriv->contactListChannels[i].channel);
            mPriv->contactListChannels[i].channel = channel;
            connect(channel->becomeReady(),
                    SIGNAL(finished(Tp::PendingOperation *)),
                    SLOT(contactListChannelReady()));
        }
    }
}

void Connection::contactListChannelReady()
{
    if (++mPriv->contactListChannelsReady ==
            ContactManager::ContactListChannel::LastType) {
        debug() << "FeatureRoster ready";
        mPriv->contactManager->setContactListChannels(mPriv->contactListChannels);
        mPriv->readinessHelper->setIntrospectCompleted(FeatureRoster, true);
    }
}

void Connection::onNewChannels(const Tp::ChannelDetailsList &channelDetailsList)
{
    QString channelType;
    uint handleType;
    foreach (const ChannelDetails &channelDetails, channelDetailsList) {
        channelType = channelDetails.properties.value(
                QLatin1String(TELEPATHY_INTERFACE_CHANNEL ".ChannelType")).toString();
        if (channelType != QLatin1String(TELEPATHY_INTERFACE_CHANNEL_TYPE_CONTACT_LIST)) {
            continue;
        }

        handleType = channelDetails.properties.value(
                QLatin1String(TELEPATHY_INTERFACE_CHANNEL ".TargetHandleType")).toUInt();
        if (handleType != Tp::HandleTypeGroup) {
            continue;
        }

        ++mPriv->featureRosterGroupsTodo; // decremented in onContactListGroupChannelReady
        ChannelPtr channel = Channel::create(ConnectionPtr(this),
                channelDetails.channel.path(), channelDetails.properties);
        mPriv->contactListGroupChannels.append(channel);
        connect(channel->becomeReady(),
                SIGNAL(finished(Tp::PendingOperation*)),
                SLOT(onContactListGroupChannelReady(Tp::PendingOperation*)));
    }
}

void Connection::onContactListGroupChannelReady(Tp::PendingOperation *op)
{
    --mPriv->featureRosterGroupsTodo; // incremented in onNewChannels

    if (!isReady(FeatureRosterGroups)) {
        mPriv->checkFeatureRosterGroupsReady();
    } else {
        PendingReady *pr = qobject_cast<PendingReady*>(op);
        ChannelPtr channel = ChannelPtr(qobject_cast<Channel*>(pr->object()));
        mPriv->contactManager->addContactListGroupChannel(channel);
        mPriv->contactListGroupChannels.removeOne(channel);
    }
}

void Connection::gotChannels(QDBusPendingCallWatcher *watcher)
{
    QDBusPendingReply<QVariant> reply = *watcher;

    --mPriv->featureRosterGroupsTodo; // incremented in introspectRosterGroups

    if (!reply.isError()) {
        debug() << "Got channels";
        onNewChannels(qdbus_cast<ChannelDetailsList>(reply.value()));
    } else {
        warning().nospace() << "Getting channels failed with " <<
            reply.error().name() << ":" << reply.error().message();
    }

    mPriv->checkFeatureRosterGroupsReady();

    watcher->deleteLater();
}

void Connection::gotBalance(QDBusPendingCallWatcher *watcher)
{
    QDBusPendingReply<QVariant> reply = *watcher;

    if (!reply.isError()) {
        debug() << "Got balance";
        mPriv->accountBalance = qdbus_cast<CurrencyAmount>(reply.value());
        mPriv->readinessHelper->setIntrospectCompleted(FeatureAccountBalance, true);
    } else {
        warning().nospace() << "Getting balance failed with " <<
            reply.error().name() << ":" << reply.error().message();
        mPriv->readinessHelper->setIntrospectCompleted(FeatureAccountBalance, false,
                reply.error().name(), reply.error().message());
    }

    watcher->deleteLater();
}

/**
 * Return the ConnectionInterface for this Connection. This
 * method is protected since the convenience methods provided by this
 * class should generally be used instead of calling D-Bus methods
 * directly.
 *
 * \return A pointer to the existing ConnectionInterface for this
 *         Connection.
 */
Client::ConnectionInterface *Connection::baseInterface() const
{
    return mPriv->baseInterface;
}

/**
 * Asynchronously creates a channel satisfying the given request.
 *
 * The request MUST contain the following keys:
 *   org.freedesktop.Telepathy.Channel.ChannelType
 *   org.freedesktop.Telepathy.Channel.TargetHandleType
 *
 * Upon completion, the reply to the request can be retrieved through the
 * returned PendingChannel object. The object also provides access to the
 * parameters with which the call was made and a signal to connect to get
 * notification of the request finishing processing. See the documentation
 * for that class for more info.
 *
 * The returned PendingChannel object should be freed using
 * its QObject::deleteLater() method after it is no longer used. However,
 * all PendingChannel objects resulting from requests to a particular
 * Connection will be freed when the Connection itself is freed. Conversely,
 * this means that the PendingChannel object should not be used after the
 * Connection is destroyed.
 *
 * \sa PendingChannel
 *
 * \param request A dictionary containing the desirable properties.
 * \return Pointer to a newly constructed PendingChannel object, tracking
 *         the progress of the request.
 */
PendingChannel *Connection::createChannel(const QVariantMap &request)
{
    if (mPriv->pendingStatus != StatusConnected) {
        warning() << "Calling createChannel with connection not yet connected";
        return new PendingChannel(ConnectionPtr(this),
                QLatin1String(TELEPATHY_ERROR_NOT_AVAILABLE),
                QLatin1String("Connection not yet connected"));
    }

    if (!interfaces().contains(QLatin1String(TELEPATHY_INTERFACE_CONNECTION_INTERFACE_REQUESTS))) {
        warning() << "Requests interface is not support by this connection";
        return new PendingChannel(ConnectionPtr(this),
                QLatin1String(TELEPATHY_ERROR_NOT_IMPLEMENTED),
                QLatin1String("Connection does not support Requests Interface"));
    }

    if (!request.contains(QLatin1String(TELEPATHY_INTERFACE_CHANNEL ".ChannelType"))) {
        return new PendingChannel(ConnectionPtr(this),
                QLatin1String(TELEPATHY_ERROR_INVALID_ARGUMENT),
                QLatin1String("Invalid 'request' argument"));
    }

    debug() << "Creating a Channel";
    PendingChannel *channel =
        new PendingChannel(ConnectionPtr(this),
                request, true);
    return channel;
}

/**
 * Asynchronously ensures a channel exists satisfying the given request.
 *
 * The request MUST contain the following keys:
 *   org.freedesktop.Telepathy.Channel.ChannelType
 *   org.freedesktop.Telepathy.Channel.TargetHandleType
 *
 * Upon completion, the reply to the request can be retrieved through the
 * returned PendingChannel object. The object also provides access to the
 * parameters with which the call was made and a signal to connect to get
 * notification of the request finishing processing. See the documentation
 * for that class for more info.
 *
 * The returned PendingChannel object should be freed using
 * its QObject::deleteLater() method after it is no longer used. However,
 * all PendingChannel objects resulting from requests to a particular
 * Connection will be freed when the Connection itself is freed. Conversely,
 * this means that the PendingChannel object should not be used after the
 * Connection is destroyed.
 *
 * \sa PendingChannel
 *
 * \param request A dictionary containing the desirable properties.
 * \return Pointer to a newly constructed PendingChannel object, tracking
 *         the progress of the request.
 */
PendingChannel *Connection::ensureChannel(const QVariantMap &request)
{
    if (mPriv->pendingStatus != StatusConnected) {
        warning() << "Calling ensureChannel with connection not yet connected";
        return new PendingChannel(ConnectionPtr(this),
                QLatin1String(TELEPATHY_ERROR_NOT_AVAILABLE),
                QLatin1String("Connection not yet connected"));
    }

    if (!interfaces().contains(QLatin1String(TELEPATHY_INTERFACE_CONNECTION_INTERFACE_REQUESTS))) {
        warning() << "Requests interface is not support by this connection";
        return new PendingChannel(ConnectionPtr(this),
                QLatin1String(TELEPATHY_ERROR_NOT_IMPLEMENTED),
                QLatin1String("Connection does not support Requests Interface"));
    }

    if (!request.contains(QLatin1String(TELEPATHY_INTERFACE_CHANNEL ".ChannelType"))) {
        return new PendingChannel(ConnectionPtr(this),
                QLatin1String(TELEPATHY_ERROR_INVALID_ARGUMENT),
                QLatin1String("Invalid 'request' argument"));
    }

    debug() << "Creating a Channel";
    PendingChannel *channel =
        new PendingChannel(ConnectionPtr(this), request, false);
    return channel;
}

/**
 * Request handles of the given type for the given entities (contacts,
 * rooms, lists, etc.).
 *
 * Upon completion, the reply to the request can be retrieved through the
 * returned PendingHandles object. The object also provides access to the
 * parameters with which the call was made and a signal to connect to to get
 * notification of the request finishing processing. See the documentation
 * for that class for more info.
 *
 * The returned PendingHandles object should be freed using
 * its QObject::deleteLater() method after it is no longer used. However,
 * all PendingHandles objects resulting from requests to a particular
 * Connection will be freed when the Connection itself is freed. Conversely,
 * this means that the PendingHandles object should not be used after the
 * Connection is destroyed.
 *
 * \sa PendingHandles
 *
 * \param handleType Type for the handles to request, as specified in
 *                   #HandleType.
 * \param names Names of the entities to request handles for.
 * \return Pointer to a newly constructed PendingHandles object, tracking
 *         the progress of the request.
 */
PendingHandles *Connection::requestHandles(uint handleType, const QStringList &names)
{
    debug() << "Request for" << names.length() << "handles of type" << handleType;

    {
        Private::HandleContext *handleContext = mPriv->handleContext;
        QMutexLocker locker(&handleContext->lock);
        handleContext->types[handleType].requestsInFlight++;
    }

    PendingHandles *pending =
        new PendingHandles(ConnectionPtr(this), handleType, names);
    return pending;
}

/**
 * Request a reference to the given handles. Handles not explicitly
 * requested (via requestHandles()) but eg. observed in a signal need to be
 * referenced to guarantee them staying valid.
 *
 * Upon completion, the reply to the operation can be retrieved through the
 * returned PendingHandles object. The object also provides access to the
 * parameters with which the call was made and a signal to connect to to get
 * notification of the request finishing processing. See the documentation
 * for that class for more info.
 *
 * The returned PendingHandles object should be freed using
 * its QObject::deleteLater() method after it is no longer used. However,
 * all PendingHandles objects resulting from requests to a particular
 * Connection will be freed when the Connection itself is freed. Conversely,
 * this means that the PendingHandles object should not be used after the
 * Connection is destroyed.
 *
 * \sa PendingHandles
 *
 * \param handleType Type of the handles given, as specified in #HandleType.
 * \param handles Handles to request a reference to.
 * \return Pointer to a newly constructed PendingHandles object, tracking
 *         the progress of the request.
 */
PendingHandles *Connection::referenceHandles(uint handleType, const UIntList &handles)
{
    debug() << "Reference of" << handles.length() << "handles of type" << handleType;

    UIntList alreadyHeld;
    UIntList notYetHeld;
    {
        Private::HandleContext *handleContext = mPriv->handleContext;
        QMutexLocker locker(&handleContext->lock);

        foreach (uint handle, handles) {
            if (handleContext->types[handleType].refcounts.contains(handle) ||
                handleContext->types[handleType].toRelease.contains(handle)) {
                alreadyHeld.push_back(handle);
            }
            else {
                notYetHeld.push_back(handle);
            }
        }
    }

    debug() << " Already holding" << alreadyHeld.size() <<
        "of the handles -" << notYetHeld.size() << "to go";

    PendingHandles *pending =
        new PendingHandles(ConnectionPtr(this), handleType, handles,
                alreadyHeld, notYetHeld);
    return pending;
}

/**
 * Start an asynchronous request that the connection be connected.
 *
 * The returned PendingOperation will finish successfully when the connection
 * has reached StatusConnected and the requested \a features are all ready, or
 * finish with an error if a fatal error occurs during that process.
 *
 * \param requestedFeatures The features which should be enabled
 * \return A PendingReady object which will emit finished
 *         when the Connection has reached StatusConnected, and initial setup
 *         for basic functionality, plus the given features, has succeeded or
 *         failed
 */
PendingReady *Connection::requestConnect(const Features &requestedFeatures)
{
    return new PendingConnect(this, requestedFeatures);
}

/**
 * Start an asynchronous request that the connection be disconnected.
 * The returned PendingOperation object will signal the success or failure
 * of this request; under normal circumstances, it can be expected to
 * succeed.
 *
 * \return A %PendingOperation, which will emit finished when the
 *         request finishes.
 */
PendingOperation *Connection::requestDisconnect()
{
    return new PendingVoid(baseInterface()->Disconnect(), this);
}

/**
 * Requests attributes for contacts. Optionally, the handles of the contacts
 * will be referenced automatically. Essentially, this method wraps
 * ConnectionInterfaceContactsInterface::GetContactAttributes(), integrating it
 * with the rest of the handle-referencing machinery.
 *
 * Upon completion, the reply to the request can be retrieved through the
 * returned PendingContactAttributes object. The object also provides access to
 * the parameters with which the call was made and a signal to connect to to get
 * notification of the request finishing processing. See the documentation for
 * that class for more info.
 *
 * If the remote object doesn't support the Contacts interface (as signified by
 * the list returned by interfaces() not containing
 * %TELEPATHY_INTERFACE_CONNECTION_INTERFACE_CONTACTS), the returned
 * PendingContactAttributes instance will fail instantly with the error
 * TELEPATHY_ERROR_NOT_IMPLEMENTED.
 *
 * Similarly, if the connection isn't both connected and ready
 * (<code>status() == StatusConnected && isReady()</code>), the returned
 * PendingContactAttributes instance will fail instantly with the
 * error %TELEPATHY_ERROR_NOT_AVAILABLE.
 *
 * This method requires Connection::FeatureCore to be enabled.
 *
 * \sa PendingContactAttributes
 *
 * \param handles A list of handles of type HandleTypeContact
 * \param interfaces D-Bus interfaces for which the client requires information
 * \param reference Whether the handles should additionally be referenced.
 * \return Pointer to a newly constructed PendingContactAttributes, tracking the
 *         progress of the request.
 */
PendingContactAttributes *Connection::contactAttributes(const UIntList &handles,
        const QStringList &interfaces, bool reference)
{
    debug() << "Request for attributes for" << handles.size() << "contacts";

    PendingContactAttributes *pending =
        new PendingContactAttributes(ConnectionPtr(this),
                handles, interfaces, reference);
    if (!isReady()) {
        warning() << "Connection::contactAttributes() used when not ready";
        pending->failImmediately(QLatin1String(TELEPATHY_ERROR_NOT_AVAILABLE),
                QLatin1String("The connection isn't ready"));
        return pending;
    } else if (mPriv->pendingStatus != StatusConnected) {
        warning() << "Connection::contactAttributes() used with status" << status() << "!= StatusConnected";
        pending->failImmediately(QLatin1String(TELEPATHY_ERROR_NOT_AVAILABLE),
                QLatin1String("The connection isn't Connected"));
        return pending;
    } else if (!this->interfaces().contains(QLatin1String(TELEPATHY_INTERFACE_CONNECTION_INTERFACE_CONTACTS))) {
        warning() << "Connection::contactAttributes() used without the remote object supporting"
                  << "the Contacts interface";
        pending->failImmediately(QLatin1String(TELEPATHY_ERROR_NOT_IMPLEMENTED),
                QLatin1String("The connection doesn't support the Contacts interface"));
        return pending;
    }

    {
        Private::HandleContext *handleContext = mPriv->handleContext;
        QMutexLocker locker(&handleContext->lock);
        handleContext->types[HandleTypeContact].requestsInFlight++;
    }

    Client::ConnectionInterfaceContactsInterface *contactsInterface =
        optionalInterface<Client::ConnectionInterfaceContactsInterface>();
    QDBusPendingCallWatcher *watcher =
        new QDBusPendingCallWatcher(contactsInterface->GetContactAttributes(handles, interfaces,
                    reference));
    pending->connect(watcher, SIGNAL(finished(QDBusPendingCallWatcher*)),
                              SLOT(onCallFinished(QDBusPendingCallWatcher*)));
    return pending;
}

QStringList Connection::contactAttributeInterfaces() const
{
    if (mPriv->pendingStatus != StatusConnected) {
        warning() << "Connection::contactAttributeInterfaces() used with status"
            << status() << "!= StatusConnected";
    } else if (!interfaces().contains(QLatin1String(TELEPATHY_INTERFACE_CONNECTION_INTERFACE_CONTACTS))) {
        warning() << "Connection::contactAttributeInterfaces() used without the remote object supporting"
                  << "the Contacts interface";
    }

    return mPriv->contactAttributeInterfaces;
}

/**
 * Return the ContactManager object for this connection.
 *
 * The contact manager is responsible for all contact handling in this
 * connection, including adding, removing, authorizing, etc.
 *
 * \return A pointer to this connection ContactManager object.
 *         The returned object is owned by this connection and should not be
 *         freed.
 */
ContactManager *Connection::contactManager() const
{
    return mPriv->contactManager;
}

void Connection::refHandle(uint type, uint handle)
{
    Private::HandleContext *handleContext = mPriv->handleContext;
    QMutexLocker locker(&handleContext->lock);

    if (handleContext->types[type].toRelease.contains(handle)) {
        handleContext->types[type].toRelease.remove(handle);
    }

    handleContext->types[type].refcounts[handle]++;
}

void Connection::unrefHandle(uint type, uint handle)
{
    Private::HandleContext *handleContext = mPriv->handleContext;
    QMutexLocker locker(&handleContext->lock);

    Q_ASSERT(handleContext->types.contains(type));
    Q_ASSERT(handleContext->types[type].refcounts.contains(handle));

    if (!--handleContext->types[type].refcounts[handle]) {
        handleContext->types[type].refcounts.remove(handle);
        handleContext->types[type].toRelease.insert(handle);

        if (!handleContext->types[type].releaseScheduled) {
            if (!handleContext->types[type].requestsInFlight) {
                debug() << "Lost last reference to at least one handle of type" <<
                    type << "and no requests in flight for that type - scheduling a release sweep";
                QMetaObject::invokeMethod(this, "doReleaseSweep",
                        Qt::QueuedConnection, Q_ARG(uint, type));
                handleContext->types[type].releaseScheduled = true;
            }
        }
    }
}

void Connection::doReleaseSweep(uint type)
{
    Private::HandleContext *handleContext = mPriv->handleContext;
    QMutexLocker locker(&handleContext->lock);

    Q_ASSERT(handleContext->types.contains(type));
    Q_ASSERT(handleContext->types[type].releaseScheduled);

    debug() << "Entering handle release sweep for type" << type;
    handleContext->types[type].releaseScheduled = false;

    if (handleContext->types[type].requestsInFlight > 0) {
        debug() << " There are requests in flight, deferring sweep to when they have been completed";
        return;
    }

    if (handleContext->types[type].toRelease.isEmpty()) {
        debug() << " No handles to release - every one has been resurrected";
        return;
    }

    debug() << " Releasing" << handleContext->types[type].toRelease.size() << "handles";

    mPriv->baseInterface->ReleaseHandles(type, handleContext->types[type].toRelease.toList());
    handleContext->types[type].toRelease.clear();
}

void Connection::handleRequestLanded(uint type)
{
    Private::HandleContext *handleContext = mPriv->handleContext;
    QMutexLocker locker(&handleContext->lock);

    Q_ASSERT(handleContext->types.contains(type));
    Q_ASSERT(handleContext->types[type].requestsInFlight > 0);

    if (!--handleContext->types[type].requestsInFlight &&
        !handleContext->types[type].toRelease.isEmpty() &&
        !handleContext->types[type].releaseScheduled) {
        debug() << "All handle requests for type" << type <<
            "landed and there are handles of that type to release - scheduling a release sweep";
        QMetaObject::invokeMethod(this, "doReleaseSweep", Qt::QueuedConnection, Q_ARG(uint, type));
        handleContext->types[type].releaseScheduled = true;
    }
}

void Connection::onSelfHandleChanged(uint handle)
{
    mPriv->selfHandle = handle;
    emit selfHandleChanged(handle);

    if (mPriv->readinessHelper->actualFeatures().contains(FeatureSelfContact)) {
        Private::introspectSelfContact(mPriv);
    }
}

void Connection::onBalanceChanged(const Tp::CurrencyAmount &value)
{
    mPriv->accountBalance = value;
    emit accountBalanceChanged(value);
}

QString ConnectionHelper::statusReasonToErrorName(Tp::ConnectionStatusReason reason,
        Tp::ConnectionStatus oldStatus)
{
    const char *errorName;

    switch (reason) {
        case ConnectionStatusReasonNoneSpecified:
            errorName = TELEPATHY_ERROR_DISCONNECTED;
            break;

        case ConnectionStatusReasonRequested:
            errorName = TELEPATHY_ERROR_CANCELLED;
            break;

        case ConnectionStatusReasonNetworkError:
            errorName = TELEPATHY_ERROR_NETWORK_ERROR;
            break;

        case ConnectionStatusReasonAuthenticationFailed:
            errorName = TELEPATHY_ERROR_AUTHENTICATION_FAILED;
            break;

        case ConnectionStatusReasonEncryptionError:
            errorName = TELEPATHY_ERROR_ENCRYPTION_ERROR;
            break;

        case ConnectionStatusReasonNameInUse:
            if (oldStatus == ConnectionStatusConnected) {
                errorName = TELEPATHY_ERROR_CONNECTION_REPLACED;
            } else {
                errorName = TELEPATHY_ERROR_ALREADY_CONNECTED;
            }
            break;

        case ConnectionStatusReasonCertNotProvided:
            errorName = TELEPATHY_ERROR_CERT_NOT_PROVIDED;
            break;

        case ConnectionStatusReasonCertUntrusted:
            errorName = TELEPATHY_ERROR_CERT_UNTRUSTED;
            break;

        case ConnectionStatusReasonCertExpired:
            errorName = TELEPATHY_ERROR_CERT_EXPIRED;
            break;

        case ConnectionStatusReasonCertNotActivated:
            errorName = TELEPATHY_ERROR_CERT_NOT_ACTIVATED;
            break;

        case ConnectionStatusReasonCertHostnameMismatch:
            errorName = TELEPATHY_ERROR_CERT_HOSTNAME_MISMATCH;
            break;

        case ConnectionStatusReasonCertFingerprintMismatch:
            errorName = TELEPATHY_ERROR_CERT_FINGERPRINT_MISMATCH;
            break;

        case ConnectionStatusReasonCertSelfSigned:
            errorName = TELEPATHY_ERROR_CERT_SELF_SIGNED;
            break;

        case ConnectionStatusReasonCertOtherError:
            errorName = TELEPATHY_ERROR_CERT_INVALID;
            break;

        default:
            errorName = TELEPATHY_ERROR_DISCONNECTED;
            break;
    }

    return QLatin1String(errorName);
}

} // Tp
