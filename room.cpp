/******************************************************************************
 * Copyright (C) 2015 Felix Rohrbach <kde@fxrh.de>
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
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include "room.h"

#include <array>

#include <QtCore/QHash>
#include <QtCore/QJsonArray>
#include <QtCore/QStringBuilder> // for efficient string concats (operator%)
#include <QtCore/QDebug>

#include "connection.h"
#include "state.h"
#include "user.h"
#include "events/event.h"
#include "events/roommessageevent.h"
#include "events/roomnameevent.h"
#include "events/roomaliasesevent.h"
#include "events/roomcanonicalaliasevent.h"
#include "events/roomtopicevent.h"
#include "events/roommemberevent.h"
#include "events/typingevent.h"
#include "events/receiptevent.h"
#include "jobs/roommessagesjob.h"

using namespace QMatrixClient;

class Room::Private
{
    public:
        /** Map of user names to users. User names potentially duplicate, hence a multi-hashmap. */
        typedef QMultiHash<QString, User*> members_map_t;
        
        Private(Room* parent): q(parent) {}

        Room* q;

        //static LogMessage* parseMessage(const QJsonObject& message);

		// This updates the room displayname field (which is the way a room should be shown in the room list)
		// It should be called whenever the list of members or the room name (m.room.name) or canonical alias change.
        void updateDisplayname();

        Connection* connection;
        QList<Event*> messageEvents;
        QString id;
        QStringList aliases;
        QString canonicalAlias;
        QString name;
        QString displayname;
        QString topic;
        JoinState joinState;
        int highlightCount;
        int notificationCount;
        members_map_t membersMap;
        QList<User*> usersTyping;
        QList<User*> membersLeft;
        QHash<User*, QString> lastReadEvent;
        QString prevBatch;
        RoomMessagesJob* roomMessagesJob;
        
        // Convenience methods to work with the membersMap and usersLeft. addMember()
        // and removeMember() emit respective Room:: signals after a succesful
        // operation.
        //void inviteUser(User* u); // We might get it at some point in time.
        void addMember(User* u);
        bool hasMember(User* u) const;
        // You can't identify a single user by displayname, only by id
        User* member(QString id) const;
        void renameMember(User* u, QString oldName);
        void removeMember(User* u);

        void getPreviousContent();

    private:
        QString calculateDisplayname() const;
        QString roomNameFromMemberNames(const QList<User*>& userlist) const;

        void insertMemberIntoMap(User* u);
        void removeMemberFromMap(QString username, User* u);
};

Room::Room(Connection* connection, QString id)
    : QObject(connection), d(new Private(this))
{
    d->id = id;
    d->connection = connection;
    d->joinState = JoinState::Join;
    d->roomMessagesJob = nullptr;
    qDebug() << "New Room:" << id;

    //connection->getMembers(this); // I don't think we need this anymore in r0.0.1
}

Room::~Room()
{
    qDebug() << "deconstructing room" << this;
    delete d;
}

QString Room::id() const
{
    return d->id;
}

QList< Event* > Room::messageEvents() const
{
    return d->messageEvents;
}

QString Room::name() const
{
    return d->name;
}

QStringList Room::aliases() const
{
    return d->aliases;
}

QString Room::canonicalAlias() const
{
    return d->canonicalAlias;
}

QString Room::displayName() const
{
    return d->displayname;
}

QString Room::topic() const
{
    return d->topic;
}

JoinState Room::joinState() const
{
    return d->joinState;
}

void Room::setJoinState(JoinState state)
{
    JoinState oldState = d->joinState;
    if( state == oldState )
        return;
    d->joinState = state;
    emit joinStateChanged(oldState, state);
}

void Room::markMessageAsRead(Event* event)
{
    d->connection->postReceipt(this, event);
}

QString Room::lastReadEvent(User* user)
{
    return d->lastReadEvent.value(user);
}

int Room::notificationCount() const
{
    return d->notificationCount;
}

void Room::resetNotificationCount()
{
    if( d->notificationCount == 0 )
        return;
    d->notificationCount = 0;
    emit notificationCountChanged(this);
}

int Room::highlightCount() const
{
    return d->highlightCount;
}

void Room::resetHighlightCount()
{
if( d->highlightCount == 0 )
        return;
    d->highlightCount = 0;
    emit highlightCountChanged(this);
}

QList< User* > Room::usersTyping() const
{
    return d->usersTyping;
}

QList< User* > Room::membersLeft() const
{
    return d->membersLeft;
}

QList< User* > Room::users() const
{
    return d->membersMap.values();
}

void Room::Private::insertMemberIntoMap(User *u)
{
    QList<User*> namesakes = membersMap.values(u->name());
    membersMap.insert(u->name(), u);
    // If there is exactly one namesake of the added user, signal member renaming
    // for that other one because the two should be disambiguated now.
    if (namesakes.size() == 1)
        emit q->memberRenamed(namesakes[0]);

    updateDisplayname();
}

void Room::Private::removeMemberFromMap(QString username, User* u)
{
    membersMap.remove(username, u);
    // If there was one namesake besides the removed user, signal member renaming
    // for it because it doesn't need to be disambiguated anymore.
    // TODO: Think about left users.
    QList<User*> formerNamesakes = membersMap.values(username);
    if (formerNamesakes.size() == 1)
        emit q->memberRenamed(formerNamesakes[0]);

    updateDisplayname();
}

void Room::Private::addMember(User *u)
{
    if (!hasMember(u))
    {
        insertMemberIntoMap(u);
        connect(u, &User::nameChanged, q, &Room::userRenamed);
        emit q->userAdded(u);
    }
}

bool Room::Private::hasMember(User* u) const
{
    return membersMap.values(u->name()).contains(u);
}

User* Room::Private::member(QString id) const
{
    User* u = connection->user(id);
    return hasMember(u) ? u : nullptr;
}

void Room::Private::renameMember(User* u, QString oldName)
{
    if (hasMember(u))
    {
        qWarning() << "Room::Private::renameMember(): the user "
                   << u->name()
                   << "is already known in the room under a new name.";
        return;
    }

    if (membersMap.values(oldName).contains(u))
    {
        removeMemberFromMap(oldName, u);
        insertMemberIntoMap(u);
        emit q->memberRenamed(u);

        updateDisplayname();
    }
}

void Room::Private::removeMember(User* u)
{
    if (hasMember(u))
    {
        if ( !membersLeft.contains(u) )
            membersLeft.append(u);
        removeMemberFromMap(u->name(), u);
        emit q->userRemoved(u);
    }
}

void Room::userRenamed(User* user, QString oldName)
{
    d->renameMember(user, oldName);
}

QString Room::roomMembername(User *u) const
{
    // See the CS spec, section 11.2.2.3

    QString username = u->name();
    if (username.isEmpty())
        return u->id();

    // Get the list of users with the same display name. Most likely,
    // there'll be one, but there's a chance there are more.
    auto namesakes = d->membersMap.values(username);
    if (namesakes.size() == 1)
        return username;

    // We expect a user to be a member of the room - but technically it is
    // possible to invoke roomMemberName() even for non-members. In such case
    // we return the name _with_ id, to stay on a safe side.
    if ( !namesakes.contains(u) )
    {
        qWarning()
            << "Room::roomMemberName(): user" << u->id()
            << "is not a member of the room" << id();
    }

    // In case of more than one namesake, disambiguate with user id.
    return username % " <" % u->id() % ">";
}

void Room::addMessage(Event* event)
{
    processMessageEvent(event);
    emit newMessage(event);
    //d->addState(event);
}

void Room::addInitialState(State* state)
{
    processStateEvent(state->event());
}

void Room::updateData(const SyncRoomData& data)
{
    if( d->prevBatch.isEmpty() )
        d->prevBatch = data.timelinePrevBatch;
    setJoinState(data.joinState);

    for( Event* stateEvent: data.state )
    {
        processStateEvent(stateEvent);
    }

    for( Event* timelineEvent: data.timeline )
    {

        processMessageEvent(timelineEvent);
        emit newMessage(timelineEvent);
        // State changes can arrive in a timeline event - try to check those.
        processStateEvent(timelineEvent);
    }

    for( Event* ephemeralEvent: data.ephemeral )
    {
        processEphemeralEvent(ephemeralEvent);
    }

    if( data.highlightCount != d->highlightCount )
    {
        d->highlightCount = data.highlightCount;
        emit highlightCountChanged(this);
    }
    if( data.notificationCount != d->notificationCount )
    {
        d->notificationCount = data.notificationCount;
        emit notificationCountChanged(this);
    }
}

void Room::getPreviousContent()
{
    d->getPreviousContent();
}

void Room::Private::getPreviousContent()
{
    if( !roomMessagesJob )
    {
        roomMessagesJob = connection->getMessages(q, prevBatch);
        connect( roomMessagesJob, &RoomMessagesJob::result, [=]() {
            if( !roomMessagesJob->error() )
            {
                for( Event* event: roomMessagesJob->events() )
                {
                    q->processMessageEvent(event);
                    emit q->newMessage(event);
                }
                prevBatch = roomMessagesJob->end();
            }
            roomMessagesJob = nullptr;
        });
    }
}

Connection* Room::connection()
{
    return d->connection;
}

void Room::processMessageEvent(Event* event)
{
    d->messageEvents.insert(findInsertionPos(d->messageEvents, event), event);
}

void Room::processStateEvent(Event* event)
{
    if( event->type() == EventType::RoomName )
    {
        if (RoomNameEvent* nameEvent = static_cast<RoomNameEvent*>(event))
        {
            d->name = nameEvent->name();
            qDebug() << "room name:" << d->name;
            d->updateDisplayname();
            emit namesChanged(this);
        } else
        {
            qDebug() <<
                "!!! event type is RoomName but the event is not RoomNameEvent";
        }
    }
    if( event->type() == EventType::RoomAliases )
    {
        RoomAliasesEvent* aliasesEvent = static_cast<RoomAliasesEvent*>(event);
        d->aliases = aliasesEvent->aliases();
        qDebug() << "room aliases:" << d->aliases;
        // No displayname update - aliases are not used to render a displayname
        emit namesChanged(this);
    }
    if( event->type() == EventType::RoomCanonicalAlias )
    {
        RoomCanonicalAliasEvent* aliasEvent = static_cast<RoomCanonicalAliasEvent*>(event);
        d->canonicalAlias = aliasEvent->alias();
        qDebug() << "room canonical alias:" << d->canonicalAlias;
        d->updateDisplayname();
        emit namesChanged(this);
    }
    if( event->type() == EventType::RoomTopic )
    {
        RoomTopicEvent* topicEvent = static_cast<RoomTopicEvent*>(event);
        d->topic = topicEvent->topic();
        emit topicChanged();
    }
    if( event->type() == EventType::RoomMember )
    {
        RoomMemberEvent* memberEvent = static_cast<RoomMemberEvent*>(event);
        User* u = d->connection->user(memberEvent->userId());
        u->processEvent(memberEvent);
        if( memberEvent->membership() == MembershipType::Join )
        {
            d->addMember(u);
        }
        else if( memberEvent->membership() == MembershipType::Leave )
        {
            d->removeMember(u);
        }
    }
}

void Room::processEphemeralEvent(Event* event)
{
    if( event->type() == EventType::Typing )
    {
        TypingEvent* typingEvent = static_cast<TypingEvent*>(event);
        d->usersTyping.clear();
        for( const QString& user: typingEvent->users() )
        {
            d->usersTyping.append(d->connection->user(user));
        }
        emit typingChanged();
    }
    if( event->type() == EventType::Receipt )
    {
        ReceiptEvent* receiptEvent = static_cast<ReceiptEvent*>(event);
        for( QString eventId: receiptEvent->events() )
        {
            QList<Receipt> receipts = receiptEvent->receiptsForEvent(eventId);
            for( Receipt r: receipts )
            {
                d->lastReadEvent.insert(d->connection->user(r.userId), eventId);
            }
        }
    }
}

QString Room::Private::roomNameFromMemberNames(const QList<User *> &userlist) const
{
    // This is part 3(i,ii,iii) in the room displayname algorithm described
    // in the CS spec (see also Room::Private::updateDisplayname() ).
    // The spec requires to sort users lexicographically by state_key (user id)
    // and use disambiguated display names of two topmost users excluding
    // the current one to render the name of the room.

    // std::array is the leanest C++ container
    std::array<User*, 2> first_two { nullptr, nullptr };
    std::partial_sort_copy(
        userlist.begin(), userlist.end(),
        first_two.begin(), first_two.end(),
        [this](const User* u1, const User* u2) {
            // Filter out the "me" user so that it never hits the room name
            return u1 != connection->user() && u1->id() < u2->id();
        }
    );

    // i. One-on-one chat. first_two[1] == connection->user() in this case.
    if (userlist.size() == 2)
        return q->roomMembername(first_two[0]);

    // ii. Two users besides the current one.
    if (userlist.size() == 3)
        return tr("%1 and %2")
                .arg(q->roomMembername(first_two[0]))
                .arg(q->roomMembername(first_two[1]));

    // iii. More users.
    if (userlist.size() > 3)
        return tr("%1 and %L2 others")
                .arg(q->roomMembername(first_two[0]))
                .arg(userlist.size() - 3);

    // userlist.size() < 2 - apparently, there's only current user in the room
    return QString();
}

QString Room::Private::calculateDisplayname() const
{
    // CS spec, section 11.2.2.5 Calculating the display name for a room
    // Numbers below refer to respective parts in the spec.

    // 1. Name (from m.room.name)
    if (!name.isEmpty()) {
        // The below two lines extend the spec. They take care of the case
        // when there are two rooms with the same name.
        // The format is unwittingly borrowed from the email address format.
        if (!canonicalAlias.isEmpty())
            return name % " <" % canonicalAlias % ">";

        return name;
    }

    // 2. Canonical alias
    if (!canonicalAlias.isEmpty())
        return canonicalAlias;

    // 3. Room members
    QString topMemberNames = roomNameFromMemberNames(membersMap.values());
    if (!topMemberNames.isEmpty())
        return topMemberNames;

    // 4. Users that previously left the room
    topMemberNames = roomNameFromMemberNames(membersLeft);
    if (!topMemberNames.isEmpty())
        return tr("Empty room (was: %1)").arg(topMemberNames);

    // 5. Fail miserably
    return tr("Empty room (%1)").arg(id);

    // Using m.room.aliases is explicitly discouraged by the spec
    //if (!aliases.empty() && !aliases.at(0).isEmpty())
    //    displayname = aliases.at(0);
}

void Room::Private::updateDisplayname()
{
    const QString old_name = displayname;
    displayname = calculateDisplayname();
    if (old_name != displayname)
        emit q->displaynameChanged(q);
}

// void Room::setAlias(QString alias)
// {
//     d->alias = alias;
//     emit aliasChanged(this);
// }
//
// bool Room::parseEvents(const QJsonObject& json)
// {
//     QList<LogMessage*> newMessages;
//     QJsonValue value = json.value("messages").toObject().value("chunk");
//     if( !value.isArray() )
//     {
//         return false;
//     }
//     QJsonArray messages = value.toArray();
//     for(const QJsonValue& val: messages )
//     {
//         if( !val.isObject() )
//             continue;
//         LogMessage* msg = Private::parseMessage(val.toObject());
//         if( msg )
//         {
//             newMessages.append(msg);
//         }
//
//     }
//     addMessages(newMessages);
//     return true;
// }
//
// bool Room::parseSingleEvent(const QJsonObject& json)
// {
//     qDebug() << "parseSingleEvent";
//     LogMessage* msg = Private::parseMessage(json);
//     if( msg )
//     {
//         addMessage(msg);
//         return true;
//     }
//     return false;
// }
//
// bool Room::parseState(const QJsonObject& json)
// {
//     QJsonValue value = json.value("state");
//     if( !value.isArray() )
//     {
//         return false;
//     }
//     QJsonArray states = value.toArray();
//     for( const QJsonValue& val: states )
//     {
//         QJsonObject state = val.toObject();
//         QString type = state.value("type").toString();
//         if( type == "m.room.aliases" )
//         {
//             QJsonArray aliases = state.value("content").toObject().value("aliases").toArray();
//             if( aliases.count() > 0 )
//             {
//                 setAlias(aliases.at(0).toString());
//             }
//         }
//     }
//     return true;
// }
//
// LogMessage* Room::Private::parseMessage(const QJsonObject& message)
// {
//     if( message.value("type") == "m.room.message" )
//     {
//         QJsonObject content = message.value("content").toObject();
//         if( content.value("msgtype").toString() != "m.text" )
//             return 0;
//         QString user = message.value("user_id").toString();
//         QString body = content.value("body").toString();
//         LogMessage* msg = new LogMessage( LogMessage::UserMessage, body, user );
//         return msg;
//     }
//     return 0;
// }

