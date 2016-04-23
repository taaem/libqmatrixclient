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

#ifndef QMATRIXCLIENT_CONNECTION_H
#define QMATRIXCLIENT_CONNECTION_H

#include <QtCore/QObject>

namespace QMatrixClient
{
    class Room;
    class User;
    class Event;
    class ConnectionPrivate;
    class ConnectionData;

    class SyncJob;
    class RoomMessagesJob;
    class PostReceiptJob;
    class MediaThumbnailJob;

    class Connection: public QObject {
            Q_OBJECT
            Q_PROPERTY(Status status READ status WRITE setStatus NOTIFY statusChanged)
        public:
            enum Status : int {
                Disconnected = 0, Connecting, Connected, Reconnecting, Failed
            };
            Q_ENUMS(Status)

            Connection(QUrl server, QObject* parent = nullptr);
            Connection();
            virtual ~Connection();

            QHash<QString, Room*> roomMap() const;
            Q_INVOKABLE virtual bool isConnected() const;
            virtual Status status() const;

            Q_INVOKABLE virtual void resolveServer( QString domain );
            Q_INVOKABLE virtual void connectToServer( QString user, QString password );
            Q_INVOKABLE virtual void connectWithToken( QString userId, QString token );
            Q_INVOKABLE virtual void reconnect();
            Q_INVOKABLE virtual void disconnectFromServer();
            Q_INVOKABLE virtual void logout();

            Q_INVOKABLE virtual SyncJob* sync(int timeout=-1);
            Q_INVOKABLE virtual void postMessage( Room* room, QString type, QString message );
            Q_INVOKABLE virtual PostReceiptJob* postReceipt( Room* room, Event* event );
            Q_INVOKABLE virtual void joinRoom( QString roomAlias );
            Q_INVOKABLE virtual void leaveRoom( Room* room );
            Q_INVOKABLE virtual void getMembers( Room* room );
            Q_INVOKABLE virtual RoomMessagesJob* getMessages( Room* room, QString from );
            virtual MediaThumbnailJob* getThumbnail( QUrl url, int requestedWidth, int requestedHeight );

            Q_INVOKABLE virtual User* user(QString userId);
            Q_INVOKABLE virtual User* user();
            Q_INVOKABLE virtual QString userId();
            Q_INVOKABLE virtual QString token();

        signals:
            void resolved();
            void connected();
            void reconnected();
            void disconnected();
            void loggedOut();

            void syncDone();
            void newRoom(Room* room);
            void joinedRoom(Room* room);

            /**
             * This signal is only used to indicate a change in internal status
             * (e.g. to reflect it in the UI). To connect any data-processing
             * functions use connected(), reconnected() and disconnected()
             * signals of the Connection class instead.
             */
            void statusChanged(Connection::Status newStatus);

            void loginError(QString error);
            void connectionError(QString error);
            void resolveError(QString error);
            //void jobError(BaseJob* job);

        protected:
            /**
             * Access the underlying ConnectionData class
             */
            ConnectionData* connectionData();

            /**
             * makes it possible for derived classes to have its own User class
             */
            virtual User* createUser(QString userId);

            /**
             * makes it possible for derived classes to have its own Room class
             */
            virtual Room* createRoom(QString roomId);
        private:
            void invokeLogin();
            void setStatus(Status newStatus);

            friend class ConnectionPrivate;
            ConnectionPrivate* d;
    };
}

#endif // QMATRIXCLIENT_CONNECTION_H
