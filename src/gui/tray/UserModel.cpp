#include "accountmanager.h"
#include "owncloudgui.h"
#include "UserModel.h"

#include <QDesktopServices>
#include <QIcon>
#include <QMessageBox>
#include <QSvgRenderer>
#include <QPainter>
#include <QPushButton>

namespace OCC {

User::User(AccountStatePtr &account, const bool &isCurrent, QObject* parent)
    : QObject(parent)
    , _account(account)
    , _isCurrentUser(isCurrent)
    , _activityModel(new ActivityListModel(_account.data()))
{
    connect(ProgressDispatcher::instance(), &ProgressDispatcher::itemCompleted,
        this, &User::slotItemCompleted);
    connect(ProgressDispatcher::instance(), &ProgressDispatcher::syncError,
        this, &User::slotAddError);
}

void User::slotAddError(const QString &folderAlias, const QString &message, ErrorCategory category)
{
    auto folderInstance = FolderMan::instance()->folder(folderAlias);
    if (!folderInstance)
        return;

    if (folderInstance->accountState() == _account.data()) {
        qCWarning(lcActivity) << "Item " << folderInstance->shortGuiLocalPath() << " retrieved resulted in " << message;

        Activity activity;
        activity._type = Activity::SyncResultType;
        activity._status = SyncResult::Error;
        activity._dateTime = QDateTime::fromString(QDateTime::currentDateTime().toString(), Qt::ISODate);
        activity._subject = message;
        activity._message = folderInstance->shortGuiLocalPath();
        activity._link = folderInstance->shortGuiLocalPath();
        activity._accName = folderInstance->accountState()->account()->displayName();
        activity._folder = folderAlias;


        if (category == ErrorCategory::InsufficientRemoteStorage) {
            ActivityLink link;
            link._label = tr("Retry all uploads");
            link._link = folderInstance->path();
            link._verb = "";
            link._isPrimary = true;
            activity._links.append(link);
        }

        // add 'other errors' to activity list
        _activityModel->addErrorToActivityList(activity);
    }
}

void User::slotItemCompleted(const QString &folder, const SyncFileItemPtr &item)
{
    auto folderInstance = FolderMan::instance()->folder(folder);

    if (!folderInstance)
        return;

    // check if we are adding it to the right account and if it is useful information (protocol errors)
    if (folderInstance->accountState() == _account.data()) {
        qCWarning(lcActivity) << "Item " << item->_file << " retrieved resulted in " << item->_errorString;

        Activity activity;
        activity._type = Activity::SyncFileItemType; //client activity
        activity._status = item->_status;
        activity._dateTime = QDateTime::currentDateTime();
        activity._message = item->_originalFile;
        activity._link = folderInstance->accountState()->account()->url();
        activity._accName = folderInstance->accountState()->account()->displayName();
        activity._file = item->_file;
        activity._folder = folder;

        if (item->_status == SyncFileItem::NoStatus || item->_status == SyncFileItem::Success) {
            qCWarning(lcActivity) << "Item " << item->_file << " retrieved successfully.";
            activity._message.prepend(" ");
            activity._message.prepend(tr("Synced"));
            _activityModel->addSyncFileItemToActivityList(activity);
        } else {
            qCWarning(lcActivity) << "Item " << item->_file << " retrieved resulted in error " << item->_errorString;
            activity._subject = item->_errorString;

            if (item->_status == SyncFileItem::Status::FileIgnored) {
                _activityModel->addIgnoredFileToList(activity);
            } else {
                // add 'protocol error' to activity list
                _activityModel->addErrorToActivityList(activity);
            }
        }
    }
}

AccountPtr User::account() const
{
    return _account->account();
}

void User::setCurrentUser(const bool &isCurrent)
{
    _isCurrentUser = isCurrent;
}

Folder *User::getFolder()
{
    foreach (Folder *folder, FolderMan::instance()->map()) {
        if (folder->accountState() == _account.data()) {
            return folder;
        }
    }
}

ActivityListModel *User::getActivityModel()
{
    return _activityModel;
}

void User::openLocalFolder()
{
#ifdef Q_OS_WIN
    QString path = "file:///" + this->getFolder()->path();
#else
    QString path = "file://" + this->getFolder()->path();
#endif
    QDesktopServices::openUrl(path);
}

void User::login() const
{
    _account->account()->resetRejectedCertificates();
    _account->signIn();
}

void User::logout() const
{
    _account->signOutByUi();
}

QString User::name() const
{
    // If davDisplayName is empty (can be several reasons, simplest is missing login at startup), fall back to username
    QString name = _account->account()->davDisplayName();
    if (name == "") {
        name = _account->account()->credentials()->user();
    }
    return name;
}

QString User::server(bool shortened) const
{
    QString serverUrl = _account->account()->url().toString();
    if (shortened) {
        serverUrl.replace(QLatin1String("https://"), QLatin1String(""));
        serverUrl.replace(QLatin1String("http://"), QLatin1String(""));
    }
    return serverUrl;
}

QImage User::avatar(bool whiteBg) const
{
    QImage img = AvatarJob::makeCircularAvatar(_account->account()->avatar());
    if (img.isNull()) {
        QImage image(128, 128, QImage::Format_ARGB32);
        image.fill(Qt::GlobalColor::transparent);
        QPainter painter(&image);

        QSvgRenderer renderer(QString(whiteBg ? ":/client/theme/black/user.svg" : ":/client/theme/white/user.svg"));
        renderer.render(&painter);

        return image;
    } else {
        return img;
    }
}

bool User::serverHasTalk() const
{
    return _account->hasTalk();
}

bool User::hasActivities() const
{
    return _account->account()->capabilities().hasActivities();
}

bool User::isCurrentUser() const
{
    return _isCurrentUser;
}

bool User::isConnected() const
{
    return (_account->connectionStatus() == AccountState::ConnectionStatus::Connected);
}

void User::removeAccount() const
{
    AccountManager::instance()->deleteAccount(_account.data());
    AccountManager::instance()->save();
}

/*-------------------------------------------------------------------------------------*/

UserModel *UserModel::_instance = nullptr;

UserModel *UserModel::instance()
{
    if (_instance == nullptr) {
        _instance = new UserModel();
    }
    return _instance;
}

UserModel::UserModel(QObject *parent)
    : QAbstractListModel(parent)
    , _currentUserId()
{
    // TODO: Remember selected user from last quit via settings file
    if (AccountManager::instance()->accounts().size() > 0) {
        buildUserList();
    }

    connect(AccountManager::instance(), &AccountManager::accountAdded,
        this, &UserModel::buildUserList);
}

void UserModel::buildUserList()
{
    for (int i = 0; i < AccountManager::instance()->accounts().size(); i++) {
        auto user = AccountManager::instance()->accounts().at(i);
        addUser(user);
    }
    if (_init) {
        _users.first()->setCurrentUser(true);
        _init = false;
    }
}

Q_INVOKABLE int UserModel::numUsers()
{
    return _users.size();
}

Q_INVOKABLE int UserModel::currentUserId()
{
    return _currentUserId;
}

Q_INVOKABLE bool UserModel::isUserConnected(const int &id)
{
    return _users[id]->isConnected();
}

Q_INVOKABLE QImage UserModel::currentUserAvatar()
{
    if (_users.count() >= 1) {
        return _users[_currentUserId]->avatar();
    } else {
        QImage image(128, 128, QImage::Format_ARGB32);
        image.fill(Qt::GlobalColor::transparent);
        QPainter painter(&image);
        QSvgRenderer renderer(QString(":/client/theme/white/user.svg"));
        renderer.render(&painter);

        return image;
    }
}

QImage UserModel::avatarById(const int &id)
{
    return _users[id]->avatar(true);
}

Q_INVOKABLE QString UserModel::currentUserName()
{
    if (_users.count() >= 1) {
        return _users[_currentUserId]->name();
    } else {
        return QString("No users");
    }
}

Q_INVOKABLE QString UserModel::currentUserServer()
{
    if (_users.count() >= 1) {
        return _users[_currentUserId]->server();
    } else {
        return QString("");
    }
}

Q_INVOKABLE bool UserModel::currentServerHasTalk()
{
    if (_users.count() >= 1) {
        return _users[_currentUserId]->serverHasTalk();
    } else {
        return false;
    }
}

void UserModel::addUser(AccountStatePtr &user, const bool &isCurrent)
{
    bool containsUser = false;
    for (int i = 0; i < _users.size(); i++) {
        if (_users[i]->account() == user->account()) {
            containsUser = true;
            continue;
        }
    }

    if (!containsUser) {
        beginInsertRows(QModelIndex(), rowCount(), rowCount());
        _users << new User(user, isCurrent);
        if (isCurrent) {
            _currentUserId = _users.indexOf(_users.last());
        }
        endInsertRows();
    }
}

int UserModel::currentUserIndex()
{
    return _currentUserId;
}

Q_INVOKABLE void UserModel::openCurrentAccountLocalFolder()
{
    _users[_currentUserId]->openLocalFolder();
}

Q_INVOKABLE void UserModel::openCurrentAccountTalk()
{
    QString url = _users[_currentUserId]->server(false) + "/apps/spreed";
    if (!(url.contains("http://") || url.contains("https://"))) {
        url = "https://" + _users[_currentUserId]->server(false) + "/apps/spreed";
    }
    QDesktopServices::openUrl(QUrl(url));
}

Q_INVOKABLE void UserModel::openCurrentAccountServer()
{
    QString url = _users[_currentUserId]->server(false);
    if (!(url.contains("http://") || url.contains("https://"))) {
        url = "https://" + _users[_currentUserId]->server(false);
    }
    QDesktopServices::openUrl(QUrl(url));
}

Q_INVOKABLE void UserModel::switchCurrentUser(const int &id)
{
    _users[_currentUserId]->setCurrentUser(false);
    _users[id]->setCurrentUser(true);
    _currentUserId = id;
    emit refreshCurrentUserGui();
    emit newUserSelected();
}

Q_INVOKABLE void UserModel::login(const int &id) {
    _users[id]->login();
    emit refreshCurrentUserGui();
}

Q_INVOKABLE void UserModel::logout(const int &id)
{
    _users[id]->logout();
    emit refreshCurrentUserGui();
}

Q_INVOKABLE void UserModel::removeAccount(const int &id)
{
    QMessageBox messageBox(QMessageBox::Question,
        tr("Confirm Account Removal"),
        tr("<p>Do you really want to remove the connection to the account <i>%1</i>?</p>"
           "<p><b>Note:</b> This will <b>not</b> delete any files.</p>")
            .arg(_users[id]->name()),
        QMessageBox::NoButton);
    QPushButton *yesButton =
        messageBox.addButton(tr("Remove connection"), QMessageBox::YesRole);
    messageBox.addButton(tr("Cancel"), QMessageBox::NoRole);

    messageBox.exec();
    if (messageBox.clickedButton() != yesButton) {
        return;
    }

    if (_users[id]->isCurrentUser() && _users.count() > 1) {
        id == 0 ? switchCurrentUser(1) : switchCurrentUser(0);
    }

    _users[id]->logout();
    _users[id]->removeAccount();

    beginRemoveRows(QModelIndex(), id, id);
    _users.removeAt(id);
    endRemoveRows();

    emit refreshCurrentUserGui();
}

int UserModel::rowCount(const QModelIndex &parent) const
{
    Q_UNUSED(parent);
    return _users.count();
}

QVariant UserModel::data(const QModelIndex &index, int role) const
{
    if (index.row() < 0 || index.row() >= _users.count()) {
        return QVariant();
    }

    if (role == NameRole) {
        return _users[index.row()]->name();
    } else if (role == ServerRole) {
        return _users[index.row()]->server();
    } else if (role == AvatarRole) {
        return _users[index.row()]->avatar();
    } else if (role == IsCurrentUserRole) {
        return _users[index.row()]->isCurrentUser();
    } else if (role == IsConnectedRole) {
        return _users[index.row()]->isConnected();
    } else if (role == IdRole) {
        return index.row();
    }
    return QVariant();
}

QHash<int, QByteArray> UserModel::roleNames() const
{
    QHash<int, QByteArray> roles;
    roles[NameRole] = "name";
    roles[ServerRole] = "server";
    roles[AvatarRole] = "avatar";
    roles[IsCurrentUserRole] = "isCurrentUser";
    roles[IsConnectedRole] = "isConnected";
    roles[IdRole] = "id";
    return roles;
}

ActivityListModel *UserModel::currentActivityModel()
{
    return _users[currentUserIndex()]->getActivityModel();
}

bool UserModel::currentUserHasActivities()
{
    return _users[currentUserIndex()]->hasActivities();
}

void UserModel::fetchCurrentActivityModel()
{
    if (_users[currentUserId()]->isConnected()) {
        _users[currentUserId()]->getActivityModel()->fetchMore(QModelIndex());
    }
}

/*-------------------------------------------------------------------------------------*/

ImageProvider::ImageProvider()
    : QQuickImageProvider(QQuickImageProvider::Image)
{
}

QImage ImageProvider::requestImage(const QString &id, QSize *size, const QSize &requestedSize)
{
    if (id == "currentUser") {
        return UserModel::instance()->currentUserAvatar();
    } else {
        int uid = id.toInt();
        return UserModel::instance()->avatarById(uid);
    }
}

}
