/*
 * Copyright (C) by Roeland Jago Douma <roeland@famdouma.nl>
 * Copyright (C) 2015 by Klaas Freitag <freitag@owncloud.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
 * for more details.
 */

#include "sharelinkwidget.h"
#include "ui_sharelinkwidget.h"
#include "account.h"
#include "capabilities.h"

#include "sharemanager.h"
#include "guiutility.h"

#include "QProgressIndicator.h"
#include <QBuffer>
#include <QClipboard>
#include <QFileInfo>
#include <QDesktopServices>
#include <QMessageBox>
#include <QMenu>
#include <QToolButton>

namespace OCC {

const char propertyShareC[] = "oc_share";

ShareLinkWidget::ShareLinkWidget(AccountPtr account,
    const QString &sharePath,
    const QString &localPath,
    SharePermissions maxSharingPermissions,
    QWidget *parent)
    : QWidget(parent)
    , _ui(new Ui::ShareLinkWidget)
    , _account(account)
    , _sharePath(sharePath)
    , _localPath(localPath)
    , _manager(0)
    , _passwordRequired(false)
    , _expiryRequired(false)
    , _namesSupported(true)
{
    _ui->setupUi(this);

    _ui->linkShares->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    _ui->linkShares->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    _ui->linkShares->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);

    //Is this a file or folder?
    QFileInfo fi(localPath);
    _isFile = fi.isFile();

    // Note: the share name cannot be longer than 64 characters
    _ui->nameLineEdit->setText(tr("Public link"));

    // the following progress indicator widgets are added to layouts which makes them
    // automatically deleted once the dialog dies.
    _pi_create = new QProgressIndicator();
    _pi_password = new QProgressIndicator();
    _pi_date = new QProgressIndicator();
    _pi_editing = new QProgressIndicator();
    _ui->horizontalLayout_create->addWidget(_pi_create);
    _ui->horizontalLayout_password->addWidget(_pi_password);
    _ui->layout_editing->addWidget(_pi_editing, 0, 2);
    _ui->horizontalLayout_expire->insertWidget(_ui->horizontalLayout_expire->count() - 1, _pi_date);

    connect(_ui->nameLineEdit, &QLineEdit::returnPressed, this, &ShareLinkWidget::slotShareNameEntered);
    connect(_ui->createShareButton, &QAbstractButton::clicked, this, &ShareLinkWidget::slotShareNameEntered);
    connect(_ui->linkShares, &QTableWidget::itemSelectionChanged, this, &ShareLinkWidget::slotShareSelectionChanged);
    connect(_ui->linkShares, &QTableWidget::itemChanged, this, &ShareLinkWidget::slotNameEdited);
    connect(_ui->checkBox_password, &QAbstractButton::clicked, this, &ShareLinkWidget::slotCheckBoxPasswordClicked);
    connect(_ui->lineEdit_password, &QLineEdit::returnPressed, this, &ShareLinkWidget::slotPasswordReturnPressed);
    connect(_ui->lineEdit_password, &QLineEdit::textChanged, this, &ShareLinkWidget::slotPasswordChanged);
    connect(_ui->pushButton_setPassword, &QAbstractButton::clicked, this, &ShareLinkWidget::slotPasswordReturnPressed);
    connect(_ui->checkBox_expire, &QAbstractButton::clicked, this, &ShareLinkWidget::slotCheckBoxExpireClicked);
    connect(_ui->calendar, &QDateTimeEdit::dateChanged, this, &ShareLinkWidget::slotExpireDateChanged);
    connect(_ui->radio_readOnly, &QAbstractButton::clicked, this, &ShareLinkWidget::slotPermissionsClicked);
    connect(_ui->radio_readWrite, &QAbstractButton::clicked, this, &ShareLinkWidget::slotPermissionsClicked);
    connect(_ui->radio_uploadOnly, &QAbstractButton::clicked, this, &ShareLinkWidget::slotPermissionsClicked);

    _ui->errorLabel->hide();

    bool sharingPossible = true;
    if (!_account->capabilities().sharePublicLink()) {
        displayError(tr("Link shares have been disabled"));
        sharingPossible = false;
    } else if (!(maxSharingPermissions & SharePermissionShare)) {
        displayError(tr("The file can not be shared because it was shared without sharing permission."));
        sharingPossible = false;
    }
    if (!sharingPossible) {
        _ui->nameLineEdit->setEnabled(false);
        _ui->createShareButton->setEnabled(false);
    }

    // Older servers don't support multiple public link shares
    if (!_account->capabilities().sharePublicLinkMultiple()) {
        _namesSupported = false;
        _ui->createShareButton->setText(tr("Create public link share"));
        _ui->nameLineEdit->hide();
        _ui->nameLineEdit->clear(); // so we don't send a name
    }

    _ui->shareProperties->setEnabled(false);

    _ui->pushButton_setPassword->setEnabled(false);
    _ui->lineEdit_password->setEnabled(false);
    _ui->pushButton_setPassword->setEnabled(false);
    _ui->checkBox_password->setText(tr("P&assword protect"));

    _ui->calendar->setMinimumDate(QDate::currentDate().addDays(1));
    _ui->calendar->setEnabled(false);

    // check if the file is already inside of a synced folder
    if (sharePath.isEmpty()) {
        // The file is not yet in an ownCloud synced folder. We could automatically
        // copy it over, but that is skipped as not all questions can be answered that
        // are involved in that, see https://github.com/owncloud/client/issues/2732
        //
        // _ui->checkBox_shareLink->setEnabled(false);
        // uploadExternalFile();
        qCWarning(lcSharing) << "Unable to share files not in a sync folder.";
        return;
    }


    // Parse capabilities

    // If password is enforced then don't allow users to disable it
    if (_account->capabilities().sharePublicLinkEnforcePassword()) {
        _ui->checkBox_password->setEnabled(false);
        _passwordRequired = true;
    }

    // If expiredate is enforced do not allow disable and set max days
    if (_account->capabilities().sharePublicLinkEnforceExpireDate()) {
        _ui->checkBox_expire->setEnabled(false);
        _ui->calendar->setMaximumDate(QDate::currentDate().addDays(
            _account->capabilities().sharePublicLinkExpireDateDays()));
        _expiryRequired = true;
    }

    // File can't have public upload set; we also hide it if the capability isn't there
    _ui->widget_editing->setVisible(
        !_isFile && _account->capabilities().sharePublicLinkAllowUpload());
    _ui->radio_uploadOnly->setVisible(
        _account->capabilities().sharePublicLinkSupportsUploadOnly());


    // Prepare sharing menu

    _linkContextMenu = new QMenu(this);
    connect(_linkContextMenu, &QMenu::triggered,
        this, &ShareLinkWidget::slotLinkContextMenuActionTriggered);
    _openLinkAction = _linkContextMenu->addAction(tr("Open link in browser"));
    _copyLinkAction = _linkContextMenu->addAction(tr("Copy link to clipboard"));
    _copyDirectLinkAction = _linkContextMenu->addAction(tr("Copy link to clipboard (direct download)"));
    _emailLinkAction = _linkContextMenu->addAction(tr("Send link by email"));
    _emailDirectLinkAction = _linkContextMenu->addAction(tr("Send link by email (direct download)"));
    _deleteLinkAction = _linkContextMenu->addAction(tr("Delete"));

    /*
     * Create the share manager and connect it properly
     */
    if (sharingPossible) {
        _manager = new ShareManager(_account, this);
        connect(_manager, &ShareManager::sharesFetched, this, &ShareLinkWidget::slotSharesFetched);
        connect(_manager, &ShareManager::linkShareCreated, this, &ShareLinkWidget::slotCreateShareFetched);
        connect(_manager, &ShareManager::linkShareRequiresPassword, this, &ShareLinkWidget::slotCreateShareRequiresPassword);
        connect(_manager, &ShareManager::serverError, this, &ShareLinkWidget::slotServerError);
    }
}

ShareLinkWidget::~ShareLinkWidget()
{
    delete _ui;
}

void ShareLinkWidget::getShares()
{
    if (_manager) {
        _manager->fetchShares(_sharePath);
    }
}

void ShareLinkWidget::slotSharesFetched(const QList<QSharedPointer<Share>> &shares)
{
    const QString versionString = _account->serverVersion();
    qCInfo(lcSharing) << versionString << "Fetched" << shares.count() << "shares";

    // Select the share that was previously selected,
    // except if an explicit override was asked for
    QString reselectShareId = _selectedShareId;
    if (!_newShareOverrideSelectionId.isEmpty()) {
        reselectShareId = _newShareOverrideSelectionId;
        _newShareOverrideSelectionId.clear();
    }

    auto table = _ui->linkShares;

    // Wipe the table without updating the ui elements, we
    // might want their state untouched if the same share ends
    // up being selected
    disconnect(table, &QTableWidget::itemSelectionChanged, this, &ShareLinkWidget::slotShareSelectionChanged);
    table->clearContents();
    table->setRowCount(0);
    connect(table, &QTableWidget::itemSelectionChanged, this, &ShareLinkWidget::slotShareSelectionChanged);

    auto deleteIcon = QIcon::fromTheme(QLatin1String("user-trash"),
        QIcon(QLatin1String(":/client/resources/delete.png")));

    foreach (auto share, shares) {
        if (share->getShareType() != Share::TypeLink) {
            continue;
        }
        auto linkShare = qSharedPointerDynamicCast<LinkShare>(share);

        // Connect all shares signals to gui slots
        connect(share.data(), &Share::serverError, this, &ShareLinkWidget::slotServerError);
        connect(share.data(), &Share::shareDeleted, this, &ShareLinkWidget::slotDeleteShareFetched);
        connect(share.data(), &Share::permissionsSet, this, &ShareLinkWidget::slotPermissionsSet);
        connect(linkShare.data(), &LinkShare::expireDateSet, this, &ShareLinkWidget::slotExpireSet);
        connect(linkShare.data(), &LinkShare::passwordSet, this, &ShareLinkWidget::slotPasswordSet);
        connect(linkShare.data(), &LinkShare::passwordSetError, this, &ShareLinkWidget::slotPasswordSetError);

        // Build the table row
        auto row = table->rowCount();
        table->insertRow(row);

        auto nameItem = new QTableWidgetItem;
        auto name = shareName(*linkShare);
        if (!_namesSupported) {
            nameItem->setFlags(nameItem->flags() & ~Qt::ItemIsEditable);
        }
        nameItem->setText(name);
        nameItem->setData(Qt::UserRole, QVariant::fromValue(linkShare));
        table->setItem(row, 0, nameItem);

        auto dotdotdotButton = new QToolButton;
        dotdotdotButton->setText("...");
        dotdotdotButton->setProperty(propertyShareC, QVariant::fromValue(linkShare));
        connect(dotdotdotButton, &QAbstractButton::clicked, this, &ShareLinkWidget::slotContextMenuButtonClicked);
        table->setCellWidget(row, 1, dotdotdotButton);

        auto deleteButton = new QToolButton;
        deleteButton->setIcon(deleteIcon);
        deleteButton->setProperty(propertyShareC, QVariant::fromValue(linkShare));
        deleteButton->setToolTip(tr("Delete link share"));
        connect(deleteButton, &QAbstractButton::clicked, this, &ShareLinkWidget::slotDeleteShareClicked);
        table->setCellWidget(row, 2, deleteButton);

        // Reestablish the previous selection
        if (reselectShareId == share->getId()) {
            table->selectRow(row);
        }
    }

    if (!selectedShare()) {
        if (table->rowCount() != 0) {
            // Select the first share by default
            table->selectRow(0);
        } else {
            // explicitly note the deselection,
            // since this was not triggered on table clear above
            slotShareSelectionChanged();
        }
    }

    if (!_namesSupported) {
        _ui->createShareButton->setEnabled(table->rowCount() == 0);
    }
}

void ShareLinkWidget::slotShareSelectionChanged()
{
    // Disable running progress indicators
    _pi_create->stopAnimation();
    _pi_editing->stopAnimation();
    _pi_date->stopAnimation();
    _pi_password->stopAnimation();

    _ui->errorLabel->hide();

    auto share = selectedShare();
    if (!share) {
        _selectedShareId.clear();
        _ui->shareProperties->setEnabled(false);
        _ui->radio_readOnly->setChecked(false);
        _ui->radio_readWrite->setChecked(false);
        _ui->radio_uploadOnly->setChecked(false);
        _ui->checkBox_expire->setChecked(false);
        _ui->checkBox_password->setChecked(false);
        return;
    }
    bool selectionUnchanged = _selectedShareId == share->getId();
    _selectedShareId = share->getId();

    _ui->shareProperties->setEnabled(true);

    _ui->checkBox_password->setEnabled(!_passwordRequired);
    _ui->checkBox_expire->setEnabled(!_expiryRequired);
    _ui->widget_editing->setEnabled(true);
    if (!_account->capabilities().sharePublicLinkAllowUpload()) {
        _ui->radio_readWrite->setEnabled(false);
        _ui->radio_uploadOnly->setEnabled(false);
    }

    // Password state
    _ui->checkBox_password->setText(tr("P&assword protect"));
    if (!selectionUnchanged) {
        if (share->isPasswordSet()) {
            _ui->checkBox_password->setChecked(true);
            _ui->lineEdit_password->setPlaceholderText("********");
            _ui->lineEdit_password->setEnabled(true);
        } else {
            _ui->checkBox_password->setChecked(false);
            _ui->lineEdit_password->setPlaceholderText(QString());
            _ui->lineEdit_password->setEnabled(false);
        }
        _ui->lineEdit_password->setText(QString());
        _ui->pushButton_setPassword->setEnabled(false);
    }

    // Expiry state
    _ui->calendar->setMinimumDate(QDate::currentDate().addDays(1));
    if (share->getExpireDate().isValid()) {
        _ui->checkBox_expire->setChecked(true);
        _ui->calendar->setDate(share->getExpireDate());
        _ui->calendar->setEnabled(true);
    } else {
        _ui->checkBox_expire->setChecked(false);
        _ui->calendar->setEnabled(false);
    }

    // Public upload state (box is hidden for files)
    if (!_isFile) {
        if (share->getPublicUpload()) {
            if (share->getShowFileListing()) {
                _ui->radio_readWrite->setChecked(true);
            } else {
                _ui->radio_uploadOnly->setChecked(true);
            }
        } else {
            _ui->radio_readOnly->setChecked(true);
        }
    }
}

void ShareLinkWidget::setExpireDate(const QDate &date)
{
    if (auto current = selectedShare()) {
        _pi_date->startAnimation();
        _ui->errorLabel->hide();
        current->setExpireDate(date);
    }
}

void ShareLinkWidget::slotExpireSet()
{
    if (sender() == selectedShare().data()) {
        slotShareSelectionChanged();
    }
}

void ShareLinkWidget::slotExpireDateChanged(const QDate &date)
{
    if (_ui->checkBox_expire->isChecked()) {
        setExpireDate(date);
    }
}

void ShareLinkWidget::slotPasswordReturnPressed()
{
    if (!_manager) {
        return;
    }
    if (!selectedShare()) {
        // If share creation requires a password, we'll be in this case
        if (_ui->lineEdit_password->text().isEmpty()) {
            _ui->lineEdit_password->setFocus();
            return;
        }

        _pi_create->startAnimation();
        _manager->createLinkShare(_sharePath, _ui->nameLineEdit->text(), _ui->lineEdit_password->text());
    } else {
        setPassword(_ui->lineEdit_password->text());
    }
    _ui->lineEdit_password->clearFocus();
}

void ShareLinkWidget::slotPasswordChanged(const QString &newText)
{
    // disable the set-password button
    _ui->pushButton_setPassword->setEnabled(newText.length() > 0);
}

void ShareLinkWidget::slotNameEdited(QTableWidgetItem *item)
{
    if (!_namesSupported) {
        return;
    }

    QString newName = item->text();
    auto share = item->data(Qt::UserRole).value<QSharedPointer<LinkShare>>();
    if (share && newName != share->getName() && newName != share->getToken()) {
        share->setName(newName);
    }
}

void ShareLinkWidget::setPassword(const QString &password)
{
    if (auto current = selectedShare()) {
        _pi_password->startAnimation();
        _ui->errorLabel->hide();

        _ui->checkBox_password->setEnabled(false);
        _ui->lineEdit_password->setEnabled(false);

        current->setPassword(password);
    }
}

void ShareLinkWidget::slotPasswordSet()
{
    auto share = selectedShare();
    if (sender() != share.data())
        return;

    _pi_password->stopAnimation();
    _ui->checkBox_password->setEnabled(true);
    _ui->lineEdit_password->setText(QString());
    if (share->isPasswordSet()) {
        _ui->lineEdit_password->setPlaceholderText("********");
        _ui->lineEdit_password->setEnabled(true);
    } else {
        _ui->lineEdit_password->setPlaceholderText(QString());
        _ui->lineEdit_password->setEnabled(false);
    }

    /*
     * When setting/deleting a password from a share the old share is
     * deleted and a new one is created. So we need to refetch the shares
     * at this point.
     *
     * NOTE: I don't see this happening with oC > 10
     */
    getShares();
}

void ShareLinkWidget::slotShareNameEntered()
{
    if (!_manager) {
        return;
    }
    _pi_create->startAnimation();
    _manager->createLinkShare(_sharePath, _ui->nameLineEdit->text(), QString());
}

void ShareLinkWidget::slotDeleteShareFetched()
{
    getShares();
}

void ShareLinkWidget::slotCreateShareFetched(const QSharedPointer<LinkShare> &share)
{
    _pi_create->stopAnimation();
    _pi_password->stopAnimation();
    _ui->nameLineEdit->clear();

    _newShareOverrideSelectionId = share->getId();
    getShares();
}

void ShareLinkWidget::slotCreateShareRequiresPassword(const QString &message)
{
    // Deselect existing shares
    _ui->linkShares->clearSelection();

    // Prepare password entry
    _pi_create->stopAnimation();
    _pi_password->stopAnimation();
    _ui->shareProperties->setEnabled(true);
    _ui->checkBox_password->setChecked(true);
    _ui->checkBox_password->setEnabled(false);
    _ui->checkBox_password->setText(tr("Public sh&aring requires a password"));
    _ui->checkBox_expire->setEnabled(false);
    _ui->widget_editing->setEnabled(false);
    if (!message.isEmpty()) {
        _ui->errorLabel->setText(message);
        _ui->errorLabel->show();
    }

    _passwordRequired = true;

    slotCheckBoxPasswordClicked();
}

void ShareLinkWidget::slotCheckBoxPasswordClicked()
{
    if (_ui->checkBox_password->checkState() == Qt::Checked) {
        _ui->lineEdit_password->setEnabled(true);
        _ui->pushButton_setPassword->setEnabled(true);
        _ui->lineEdit_password->setPlaceholderText(tr("Please Set Password"));
        _ui->lineEdit_password->setFocus();
    } else {
        setPassword(QString());
        _ui->lineEdit_password->setPlaceholderText(QString());
        _pi_password->startAnimation();
        _ui->lineEdit_password->setEnabled(false);
        _ui->pushButton_setPassword->setEnabled(false);
    }
}

void ShareLinkWidget::slotCheckBoxExpireClicked()
{
    if (_ui->checkBox_expire->checkState() == Qt::Checked) {
        const QDate date = QDate::currentDate().addDays(1);
        setExpireDate(date);
        _ui->calendar->setDate(date);
        _ui->calendar->setMinimumDate(date);
        _ui->calendar->setEnabled(true);
    } else {
        setExpireDate(QDate());
        _ui->calendar->setEnabled(false);
    }
}

void ShareLinkWidget::emailShareLink(const QUrl &url)
{
    QString fileName = _sharePath.mid(_sharePath.lastIndexOf('/') + 1);
    Utility::openEmailComposer(
        tr("I shared %1 with you").arg(fileName),
        url.toString(),
        this);
}

void ShareLinkWidget::openShareLink(const QUrl &url)
{
    Utility::openBrowser(url, this);
}

void ShareLinkWidget::confirmAndDeleteShare(const QSharedPointer<LinkShare> &share)
{
    auto messageBox = new QMessageBox(
        QMessageBox::Question,
        tr("Confirm Link Share Deletion"),
        tr("<p>Do you really want to delete the public link share <i>%1</i>?</p>"
           "<p>Note: This action cannot be undone.</p>")
            .arg(shareName(*share)),
        QMessageBox::NoButton,
        this);
    QPushButton *yesButton =
        messageBox->addButton(tr("Delete"), QMessageBox::YesRole);
    messageBox->addButton(tr("Cancel"), QMessageBox::NoRole);

    connect(messageBox, &QMessageBox::finished, this,
        [messageBox, yesButton, share]() {
        if (messageBox->clickedButton() == yesButton)
            share->deleteShare();
    });
    messageBox->open();
}

QString ShareLinkWidget::shareName(const LinkShare &share) const
{
    QString name = share.getName();
    if (!name.isEmpty())
        return name;
    if (!_namesSupported)
        return tr("Public link");
    return share.getToken();
}

void ShareLinkWidget::slotContextMenuButtonClicked()
{
    auto share = sender()->property(propertyShareC).value<QSharedPointer<LinkShare>>();
    bool downloadEnabled = share->getShowFileListing();
    _copyDirectLinkAction->setVisible(downloadEnabled);
    _emailDirectLinkAction->setVisible(downloadEnabled);

    _linkContextMenu->setProperty(propertyShareC, QVariant::fromValue(share));
    _linkContextMenu->exec(QCursor::pos());
}

void ShareLinkWidget::slotLinkContextMenuActionTriggered(QAction *action)
{
    auto share = sender()->property(propertyShareC).value<QSharedPointer<LinkShare>>();

    if (action == _deleteLinkAction) {
        confirmAndDeleteShare(share);
    } else if (action == _copyLinkAction) {
        QApplication::clipboard()->setText(share->getLink().toString());
    } else if (action == _copyDirectLinkAction) {
        QApplication::clipboard()->setText(share->getDirectDownloadLink().toString());
    } else if (action == _emailLinkAction) {
        emailShareLink(share->getLink());
    } else if (action == _emailDirectLinkAction) {
        emailShareLink(share->getDirectDownloadLink());
    } else if (action == _openLinkAction) {
        openShareLink(share->getLink());
    }
}

void ShareLinkWidget::slotDeleteShareClicked()
{
    auto share = sender()->property(propertyShareC).value<QSharedPointer<LinkShare>>();
    confirmAndDeleteShare(share);
}

void ShareLinkWidget::slotPermissionsClicked()
{
    if (auto current = selectedShare()) {
        _ui->widget_editing->setEnabled(false);
        _pi_editing->startAnimation();
        _ui->errorLabel->hide();

        SharePermissions perm = SharePermissionRead;
        if (_ui->radio_readWrite->isChecked()) {
            perm = SharePermissionRead | SharePermissionCreate
                | SharePermissionUpdate | SharePermissionDelete;
        } else if (_ui->radio_uploadOnly->isChecked()) {
            perm = SharePermissionCreate;
        }
        current->setPermissions(perm);
    }
}

QSharedPointer<LinkShare> ShareLinkWidget::selectedShare() const
{
    const auto items = _ui->linkShares->selectedItems();
    if (items.isEmpty()) {
        return QSharedPointer<LinkShare>();
    }

    return items.first()->data(Qt::UserRole).value<QSharedPointer<LinkShare>>();
}

void ShareLinkWidget::slotPermissionsSet()
{
    if (sender() == selectedShare().data()) {
        slotShareSelectionChanged();
    }
}

void ShareLinkWidget::slotServerError(int code, const QString &message)
{
    _pi_create->stopAnimation();
    _pi_date->stopAnimation();
    _pi_password->stopAnimation();
    _pi_editing->stopAnimation();

    qCWarning(lcSharing) << "Error from server" << code << message;
    displayError(message);
}

void ShareLinkWidget::slotPasswordSetError(int code, const QString &message)
{
    slotServerError(code, message);

    _ui->checkBox_password->setEnabled(!_passwordRequired);
    _ui->lineEdit_password->setEnabled(true);
    _ui->lineEdit_password->setFocus();
}

void ShareLinkWidget::displayError(const QString &errMsg)
{
    _ui->errorLabel->setText(errMsg);
    _ui->errorLabel->show();
}
}
