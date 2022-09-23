// Copyright (c) 2011-2015 The Bitcoin Core developers
// Copyright (c) 2014-2022 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/overviewpage.h>
#include <qt/forms/ui_overviewpage.h>

#include <qt/bitcoinunits.h>
#include <qt/clientmodel.h>
#include <qt/guiutil.h>
#include <qt/optionsmodel.h>
#include <qt/transactionfilterproxy.h>
#include <qt/transactiontablemodel.h>
#include <qt/utilitydialog.h>
#include <qt/walletmodel.h>

#include <QAbstractItemDelegate>
#include <QPainter>
#include <QSettings>
#include <QTimer>

#define ITEM_HEIGHT 54
#define NUM_ITEMS_DISABLED 5
#define NUM_ITEMS_ENABLED_NORMAL 6
#define NUM_ITEMS_ENABLED_ADVANCED 8

Q_DECLARE_METATYPE(interfaces::WalletBalances)

class TxViewDelegate : public QAbstractItemDelegate
{
    Q_OBJECT
public:
    explicit TxViewDelegate(QObject* parent = nullptr) :
        QAbstractItemDelegate(), unit(BitcoinUnits::KII)
    {

    }

    inline void paint(QPainter *painter, const QStyleOptionViewItem &option,
                      const QModelIndex &index ) const
    {
        painter->save();

        QRect mainRect = option.rect;
        int xspace = 8;
        int ypad = 8;
        int halfheight = (mainRect.height() - 2*ypad)/2;
        QRect rectTopHalf(mainRect.left() + xspace, mainRect.top() + ypad, mainRect.width() - xspace, halfheight);
        QRect rectBottomHalf(mainRect.left() + xspace, mainRect.top() + ypad + halfheight + 5, mainRect.width() - xspace, halfheight);
        QRect rectBounding;
        QColor colorForeground;
        qreal initialFontSize = painter->font().pointSizeF();

        // Grab model indexes for desired data from TransactionTableModel
        QModelIndex indexDate = index.sibling(index.row(), TransactionTableModel::Date);
        QModelIndex indexAmount = index.sibling(index.row(), TransactionTableModel::Amount);
        QModelIndex indexAddress = index.sibling(index.row(), TransactionTableModel::ToAddress);

        // Draw first line (with slightly bigger font than the second line will get)
        // Content: Date/Time, Optional IS indicator, Amount
        painter->setFont(GUIUtil::getFont(GUIUtil::FontWeight::Normal, false, GUIUtil::getScaledFontSize(initialFontSize * 1.17)));
        // Date/Time
        colorForeground = qvariant_cast<QColor>(indexDate.data(Qt::ForegroundRole));
        QString strDate = indexDate.data(Qt::DisplayRole).toString();
        painter->setPen(colorForeground);
        painter->drawText(rectTopHalf, Qt::AlignLeft | Qt::AlignVCenter, strDate, &rectBounding);
        // Optional IS indicator
        QIcon iconInstantSend = qvariant_cast<QIcon>(indexAddress.data(TransactionTableModel::RawDecorationRole));
        QRect rectInstantSend(rectBounding.right() + 5, rectTopHalf.top(), 16, halfheight);
        iconInstantSend.paint(painter, rectInstantSend);
        // Amount
        colorForeground = qvariant_cast<QColor>(indexAmount.data(Qt::ForegroundRole));
        // Note: do NOT use Qt::DisplayRole, have format properly here
        qint64 nAmount = index.data(TransactionTableModel::AmountRole).toLongLong();
        QString strAmount = BitcoinUnits::floorWithUnit(unit, nAmount, true, BitcoinUnits::separatorAlways);
        painter->setPen(colorForeground);
        painter->drawText(rectTopHalf, Qt::AlignRight | Qt::AlignVCenter, strAmount);

        // Draw second line (with the initial font)
        // Content: Address/label, Optional Watchonly indicator
        painter->setFont(GUIUtil::getFont(GUIUtil::FontWeight::Normal, false, GUIUtil::getScaledFontSize(initialFontSize)));
        // Address/Label
        colorForeground = qvariant_cast<QColor>(indexAddress.data(Qt::ForegroundRole));
        QString address = indexAddress.data(Qt::DisplayRole).toString();
        painter->setPen(colorForeground);
        painter->drawText(rectBottomHalf, Qt::AlignLeft | Qt::AlignVCenter, address, &rectBounding);
        // Optional Watchonly indicator
        if (index.data(TransactionTableModel::WatchonlyRole).toBool())
        {
            QIcon iconWatchonly = qvariant_cast<QIcon>(index.data(TransactionTableModel::WatchonlyDecorationRole));
            QRect rectWatchonly(rectBounding.right() + 5, rectBottomHalf.top(), 16, halfheight);
            iconWatchonly.paint(painter, rectWatchonly);
        }

        painter->restore();
    }

    inline QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const
    {
        return QSize(ITEM_HEIGHT, ITEM_HEIGHT);
    }

    int unit;

};
#include <qt/overviewpage.moc>

OverviewPage::OverviewPage(QWidget* parent) :
    QWidget(parent),
    timer(nullptr),
    ui(new Ui::OverviewPage),
    clientModel(0),
    walletModel(0),
    cachedNumISLocks(-1),
    txdelegate(new TxViewDelegate(this))
{
    ui->setupUi(this);

    GUIUtil::setFont({ui->label_4,
                      ui->label_5,

                     }, GUIUtil::FontWeight::Bold, 16);

    GUIUtil::setFont({ui->labelTotalText,
                      ui->labelWatchTotal,
                      ui->labelTotal
                     }, GUIUtil::FontWeight::Bold, 14);

    GUIUtil::setFont({ui->labelBalanceText,
                      ui->labelPendingText,
                      ui->labelImmatureText,
                      ui->labelWatchonly,
                      ui->labelSpendable
                     }, GUIUtil::FontWeight::Bold);

    GUIUtil::updateFonts();

    m_balances.balance = -1;

    // Recent transactions
    ui->listTransactions->setItemDelegate(txdelegate);
    // Note: minimum height of listTransactions will be set later in updateAdvancedCJUI() to reflect actual settings
    ui->listTransactions->setAttribute(Qt::WA_MacShowFocusRect, false);

    connect(ui->listTransactions, SIGNAL(clicked(QModelIndex)), this, SLOT(handleTransactionClicked(QModelIndex)));

    // init "out of sync" warning labels
    ui->labelWalletStatus->setText("(" + tr("out of sync") + ")");

    ui->labelTransactionsStatus->setText("(" + tr("out of sync") + ")");

    // start with displaying the "out of sync" warnings
    showOutOfSyncWarning(true);

    timer = new QTimer(this);

}

void OverviewPage::handleTransactionClicked(const QModelIndex &index)
{
    if(filter)
        Q_EMIT transactionClicked(filter->mapToSource(index));
}

void OverviewPage::handleOutOfSyncWarningClicks()
{
    Q_EMIT outOfSyncWarningClicked();
}

OverviewPage::~OverviewPage()
{
    delete ui;
}

void OverviewPage::setBalance(const interfaces::WalletBalances& balances)
{
    int unit = walletModel->getOptionsModel()->getDisplayUnit();
    m_balances = balances;
    ui->labelBalance->setText(BitcoinUnits::floorHtmlWithUnit(unit, balances.balance, false, BitcoinUnits::separatorAlways));
    ui->labelUnconfirmed->setText(BitcoinUnits::floorHtmlWithUnit(unit, balances.unconfirmed_balance, false, BitcoinUnits::separatorAlways));
    ui->labelImmature->setText(BitcoinUnits::floorHtmlWithUnit(unit, balances.immature_balance, false, BitcoinUnits::separatorAlways));
    
    ui->labelTotal->setText(BitcoinUnits::floorHtmlWithUnit(unit, balances.balance + balances.unconfirmed_balance + balances.immature_balance, false, BitcoinUnits::separatorAlways));
    ui->labelWatchAvailable->setText(BitcoinUnits::floorHtmlWithUnit(unit, balances.watch_only_balance, false, BitcoinUnits::separatorAlways));
    ui->labelWatchPending->setText(BitcoinUnits::floorHtmlWithUnit(unit, balances.unconfirmed_watch_only_balance, false, BitcoinUnits::separatorAlways));
    ui->labelWatchImmature->setText(BitcoinUnits::floorHtmlWithUnit(unit, balances.immature_watch_only_balance, false, BitcoinUnits::separatorAlways));
    ui->labelWatchTotal->setText(BitcoinUnits::floorHtmlWithUnit(unit, balances.watch_only_balance + balances.unconfirmed_watch_only_balance + balances.immature_watch_only_balance, false, BitcoinUnits::separatorAlways));

    // only show immature (newly mined) balance if it's non-zero, so as not to complicate things
    // for the non-mining users
    bool fDebugUI = gArgs.GetBoolArg("-debug-ui", false);
    bool showImmature = fDebugUI || balances.immature_balance != 0;
    bool showWatchOnlyImmature = fDebugUI || balances.immature_watch_only_balance != 0;

    // for symmetry reasons also show immature label when the watch-only one is shown
    ui->labelImmature->setVisible(showImmature || showWatchOnlyImmature);
    ui->labelImmatureText->setVisible(showImmature || showWatchOnlyImmature);
    ui->labelWatchImmature->setVisible(showWatchOnlyImmature); // show watch-only immature balance

    if (walletModel) {
        int numISLocks = walletModel->getNumISLocks();
        if(cachedNumISLocks != numISLocks) {
            cachedNumISLocks = numISLocks;
            ui->listTransactions->update();
        }
    }
}

// show/hide watch-only labels
void OverviewPage::updateWatchOnlyLabels(bool showWatchOnly)
{
    ui->labelSpendable->setVisible(showWatchOnly);      // show spendable label (only when watch-only is active)
    ui->labelWatchonly->setVisible(showWatchOnly);      // show watch-only label
    ui->lineWatchBalance->setVisible(showWatchOnly);    // show watch-only balance separator line
    ui->labelWatchAvailable->setVisible(showWatchOnly); // show watch-only available balance
    ui->labelWatchPending->setVisible(showWatchOnly);   // show watch-only pending balance
    ui->labelWatchTotal->setVisible(showWatchOnly);     // show watch-only total balance

    if (!showWatchOnly){
        ui->labelWatchImmature->hide();
    }
    else{
        ui->labelBalance->setIndent(20);
        ui->labelUnconfirmed->setIndent(20);
        ui->labelImmature->setIndent(20);
        ui->labelTotal->setIndent(20);
    }
}

void OverviewPage::setClientModel(ClientModel *model)
{
    this->clientModel = model;
    if(model)
    {
        // Show warning if this is a prerelease version
        connect(model, SIGNAL(alertsChanged(QString)), this, SLOT(updateAlerts(QString)));
        updateAlerts(model->getStatusBarWarnings());
    }
}

void OverviewPage::setWalletModel(WalletModel *model)
{
    this->walletModel = model;
    if(model && model->getOptionsModel())
    {
        // update the display unit, to not use the default ("KII")
        updateDisplayUnit();
        // Keep up to date with wallet
        interfaces::Wallet& wallet = model->wallet();
        interfaces::WalletBalances balances = wallet.getBalances();
        setBalance(balances);
        connect(model, SIGNAL(balanceChanged(interfaces::WalletBalances)), this, SLOT(setBalance(interfaces::WalletBalances)));

        connect(model->getOptionsModel(), SIGNAL(displayUnitChanged(int)), this, SLOT(updateDisplayUnit()));
        updateWatchOnlyLabels(wallet.haveWatchOnly() || gArgs.GetBoolArg("-debug-ui", false));
        connect(model, SIGNAL(notifyWatchonlyChanged(bool)), this, SLOT(updateWatchOnlyLabels(bool)));

        // explicitly update PS frame and transaction list to reflect actual settings
        updateAdvancedCJUI(model->getOptionsModel()->getShowAdvancedCJUI());

    }
}

void OverviewPage::updateDisplayUnit()
{
    if(walletModel && walletModel->getOptionsModel())
    {
        nDisplayUnit = walletModel->getOptionsModel()->getDisplayUnit();
        if (m_balances.balance != -1) {
            setBalance(m_balances);
        }

        // Update txdelegate->unit with the current unit
        txdelegate->unit = nDisplayUnit;

        ui->listTransactions->update();
    }
}

void OverviewPage::updateAlerts(const QString &warnings)
{
    this->ui->labelAlerts->setVisible(!warnings.isEmpty());
    this->ui->labelAlerts->setText(warnings);
}

void OverviewPage::showOutOfSyncWarning(bool fShow)
{
    ui->labelWalletStatus->setVisible(fShow);

    ui->labelTransactionsStatus->setVisible(fShow);
}

void OverviewPage::updateAdvancedCJUI(bool fShowAdvancedCJUI)
{
    if (!walletModel || !clientModel) return;

    this->fShowAdvancedCJUI = fShowAdvancedCJUI;

}

void OverviewPage::SetupTransactionList(int nNumItems)
{
    if (walletModel == nullptr || walletModel->getTransactionTableModel() == nullptr) {
        return;
    }

    // Set up transaction list
    if (filter == nullptr) {
        filter.reset(new TransactionFilterProxy());
        filter->setSourceModel(walletModel->getTransactionTableModel());
        filter->setDynamicSortFilter(true);
        filter->setSortRole(Qt::EditRole);
        filter->setShowInactive(false);
        filter->sort(TransactionTableModel::Date, Qt::DescendingOrder);
        ui->listTransactions->setModel(filter.get());
    }

    if (filter->rowCount() == nNumItems) {
        return;
    }

    filter->setLimit(nNumItems);
    ui->listTransactions->setMinimumHeight(nNumItems * ITEM_HEIGHT);
}


