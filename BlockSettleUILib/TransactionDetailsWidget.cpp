#include "TransactionDetailsWidget.h"
#include "ui_TransactionDetailsWidget.h"
#include "BTCNumericTypes.h"
#include "BlockObj.h"
#include "UiUtils.h"

#include <memory>
#include <QToolTip>

Q_DECLARE_METATYPE(Tx);

const BinaryData BinaryTXID::getRPCTXID() {
   BinaryData retVal;
   if(txidIsRPC_ == true) {
      retVal = txid_;
   }
   else {
      retVal = txid_.swapEndian();
   }

   return retVal;
}

const BinaryData BinaryTXID::getInternalTXID() {
   BinaryData retVal;
   if(txidIsRPC_ == false) {
      retVal = txid_;
   }
   else {
      retVal = txid_.swapEndian();
   }

   return retVal;
}

bool BinaryTXID::operator==(const BinaryTXID& inTXID) const {
   return (txidIsRPC_ == inTXID.getTXIDIsRPC()) &&
          (txid_ == inTXID.getRawTXID());
}

bool BinaryTXID::operator<(const BinaryTXID& inTXID) const {
   return txid_ < inTXID.getRawTXID();
}

bool BinaryTXID::operator>(const BinaryTXID& inTXID) const {
   return txid_ > inTXID.getRawTXID();
}

TransactionDetailsWidget::TransactionDetailsWidget(QWidget *parent) :
    QWidget(parent),
   ui_(new Ui::TransactionDetailsWidget)
{
   ui_->setupUi(this);

   // setting up a tooltip that pops up immediately when mouse hovers over it
   QIcon btcIcon(QLatin1String(":/resources/notification_info.png"));
   ui_->labelTxPopup->setPixmap(btcIcon.pixmap(13, 13));
   ui_->labelTxPopup->setMouseTracking(true);
   ui_->labelTxPopup->toolTip_ = tr("The Transaction ID (TXID) uses RPC byte "
                                    "order. It will match the RPC output from "
                                    "Bitcoin Core, along with the byte order "
                                    "from the BlockSettle Terminal.");

   // set the address column to have hand cursor
   ui_->treeInput->handCursorColumns_.append(colAddressId);
   ui_->treeOutput->handCursorColumns_.append(colAddressId);
   // allow address columns to be copied to clipboard with right click
   ui_->treeInput->copyToClipboardColumns_.append(colAddressId);
   ui_->treeOutput->copyToClipboardColumns_.append(colAddressId);

   connect(ui_->treeInput, &QTreeWidget::itemClicked,
      this, &TransactionDetailsWidget::onAddressClicked);
   connect(ui_->treeOutput, &QTreeWidget::itemClicked,
      this, &TransactionDetailsWidget::onAddressClicked);
}

TransactionDetailsWidget::~TransactionDetailsWidget()
{
    delete ui_;
}

// Initialize the widget and related widgets (block, address, Tx)
void TransactionDetailsWidget::init(const std::shared_ptr<ArmoryConnection> &armory,
                                    const std::shared_ptr<spdlog::logger> &inLogger)
{
   armory_ = armory;
   logger_ = inLogger;
}

// This function uses getTxByHash() to retrieve info about transaction. The
// incoming TXID must be in RPC order, not internal order.
void TransactionDetailsWidget::populateTransactionWidget(BinaryTXID rpcTXID,
                                                         const bool& firstPass) {
   // get the transaction data from armory
   const auto &cbTX = [this, &rpcTXID, firstPass](Tx tx) {
      if (!tx.isInitialized()) {
         logger_->error("[TransactionDetailsWidget::populateTransactionWidget] TX not " \
                        "initialized for hash {}.",
                        rpcTXID.getRPCTXID().toHexStr());
         // If failure, try swapping the endian. Treat the result as RPC.
         if(firstPass == true) {
            BinaryTXID intTXID(rpcTXID.getInternalTXID(), true);
            populateTransactionWidget(intTXID, false);
         }
         return;
      }

      // UI changes in a non-main thread will trigger a crash. Invoke a new
      // thread to handle the received data. (UI changes happen eventually.)
      QMetaObject::invokeMethod(this, [this, tx] { processTxData(tx); });
   };

   // The TXID passed to Armory *must* be in internal order!
   armory_->getTxByHash(rpcTXID.getInternalTXID(), cbTX);
}

// Used in callback to process the Tx object returned by Armory.
void TransactionDetailsWidget::processTxData(Tx tx) {
   // Save Tx and the prev Tx entries (get input amounts & such)
   curTx_ = tx;

   // Get each Tx object associated with the Tx's TxIn object. Needed to calc
   // the fees.
   const auto &cbProcessTX = [this](std::vector<Tx> prevTxs) {
      for (const auto &prevTx : prevTxs) {
         BinaryTXID intPrevTXHash(prevTx.getThisHash(), false);
         prevTxHashSet_.erase(intPrevTXHash.getInternalTXID());
         prevTxMap_[intPrevTXHash] = prevTx;
      }

      // We're ready to display all the transaction-related data in the UI.
      setTxGUIValues();
   };

   // While here, we need to get the prev Tx with the UTXO being spent.
   // This is done so that we can calculate fees later.
   for (size_t i = 0; i < tx.getNumTxIn(); i++) {
      TxIn in = tx.getTxInCopy(i);
      OutPoint op = in.getOutPoint();
      BinaryTXID intPrevTXID(op.getTxHash(), false);
      const auto &itTX = prevTxMap_.find(intPrevTXID);
      if(itTX == prevTxMap_.end()) {
         prevTxHashSet_.insert(intPrevTXID.getInternalTXID());
      }
   }

   // Get the TxIn-associated Tx objects from Armory.
   if(!prevTxHashSet_.empty()) {
      armory_->getTXsByHash(prevTxHashSet_, cbProcessTX);
   }
}

// NB: Don't use ClientClasses::BlockHeader. It has parsing capabilities but
// it's meant to be an internal Armory class, touching things like the DB. Just
// parse the raw data header here.
void TransactionDetailsWidget::getHeaderData(const BinaryData& inHeader)
{
   if(inHeader.getSize() != 80)
   {
      logger_->error("[TransactionDetailWidgets::getHeaderData] Header is " \
                     "not the correct size - size = {}", inHeader.getSize());
         return;
   }

   // FIX - May want to rethink where the data is saved.
/*   curTxVersion = READ_UINT32_LE(inHeader.getPtr());
   curTxPrevHash = BinaryData(inHeader.getPtr() + 4, 32);
   curTxMerkleRoot = BinaryData(inHeader.getPtr() + 36, 32);
   curTxTimestamp = READ_UINT32_LE(inHeader.getPtr() + 68);
   curTxDifficulty = BinaryData(inHeader.getPtr() + 72, 4);
   curTxNonce = READ_UINT32_LE(inHeader.getPtr() + 76);*/
}

void TransactionDetailsWidget::setTxGUIValues()
{
   // In case we've been here earlier, clear all the text.
   clearFields();

   // Get Tx header data. NOT USED FOR NOW.
//   BinaryData txHdr(curTx_.getPtr(), 80);
//   getHeaderData(txHdr);

   // Get fees & fee/byte by looping through the prev Tx set and calculating.
   uint64_t totIn = 0;
   for(size_t r = 0; r < curTx_.getNumTxIn(); ++r) {
      TxIn in = curTx_.getTxInCopy(r);
      OutPoint op = in.getOutPoint();
      BinaryTXID intPrevTXID(op.getTxHash(), false);
      const auto &prevTx = prevTxMap_[intPrevTXID];
      if (prevTx.isInitialized()) {
         TxOut prevOut = prevTx.getTxOutCopy(op.getTxOutIndex());
         totIn += prevOut.getValue();
      }
   }
   uint64_t fees = totIn - curTx_.getSumOfOutputs();
   double feePerByte = (double)fees / (double)curTx_.getSize();

   // NB: Certain data (timestamp, height, and # of confs) can't be obtained
   // from the Tx object. For now, we're leaving placeholders until a solution
   // can be found. In theory, the timestamp can be obtained from the timestamp.
   // The header data retrieved right now seems to be inaccurate, so we're not
   // using that right now.

   // Populate the GUI fields.
   // Output TXID in RPC byte order by flipping TXID bytes rcv'd by Armory (internal
   // order).
   ui_->tranID->setText(QString::fromStdString(curTx_.getThisHash().toHexStr(true)));
//   ui_->tranDate->setText(UiUtils::displayDateTime(QDateTime::fromTime_t(curTxTimestamp)));
   ui_->tranDate->setText(QString::fromStdString("Timestamp here"));        // FIX ME!!!
   ui_->tranHeight->setText(QString::fromStdString("Height here"));         // FIX ME!!!
   ui_->tranConfirmations->setText(QString::fromStdString("# confs here")); // FIX ME!!!
   ui_->tranNumInputs->setText(QString::number(curTx_.getNumTxIn()));
   ui_->tranNumOutputs->setText(QString::number(curTx_.getNumTxOut()));
   ui_->tranOutput->setText(QString::number(curTx_.getSumOfOutputs() / BTCNumericTypes::BalanceDivider,
                                            'f',
                                            BTCNumericTypes::default_precision));
   ui_->tranFees->setText(QString::number(fees / BTCNumericTypes::BalanceDivider,
                                          'f',
                                          BTCNumericTypes::default_precision));
   ui_->tranFeePerByte->setText(QString::number(nearbyint(feePerByte)));
   ui_->tranSize->setText(QString::number(curTx_.getSize()));

   loadInputs();
}

// This function populates the inputs tree with top level and child items.
// The exact same code applies to ui_->treeOutput.
void TransactionDetailsWidget::loadInputs() {
   // for testing purposes i populate both trees with test data
   loadTreeIn(ui_->treeInput);
   loadTreeOut(ui_->treeOutput);
}

// Input widget population.
void TransactionDetailsWidget::loadTreeIn(CustomTreeWidget *tree) {
   tree->clear();

   // here's the code to add data to the Input tree.
   for (size_t i = 0; i < curTx_.getNumTxIn(); i++) {
      TxOut prevOut;
      TxIn in = curTx_.getTxInCopy(i);
      OutPoint op = in.getOutPoint();
      BinaryTXID intPrevTXID(op.getTxHash(), false);
      const auto &prevTx = prevTxMap_[intPrevTXID];
      if (prevTx.isInitialized()) {
         prevOut = prevTx.getTxOutCopy(op.getTxOutIndex());
      }
      const auto outAddr = bs::Address::fromTxOut(prevOut);
      double amtBTC = prevOut.getValue() / BTCNumericTypes::BalanceDivider;

      // create a top level item using type, address, amount, wallet values
      QTreeWidgetItem *item = createItem(tree,
                                         tr("Input"),
                                         outAddr.display(),
                                         QString::number(amtBTC,
                                                         'f',
                                                         BTCNumericTypes::default_precision),
                                         QString());

      // Example: Add several child items to this top level item to crate a new
      // branch in the tree.
/*      item->addChild(createItem(item,
                                tr("1JSAGsDo56rEqgxf3R1EAiCgwGJCUB31Cr"),
                                tr("1JSAGsDo56rEqgxf3R1EAiCgwGJCUB31Cr"),
                                tr("-0.00850000"),
                                tr("Settlement")));
      item->setExpanded(true);*/

      // add the item to the tree
      tree->addTopLevelItem(item);
   }
   tree->resizeColumns();
}

// Output widget population.
void TransactionDetailsWidget::loadTreeOut(CustomTreeWidget *tree) {
   tree->clear();

   // here's the code to add data to the Input tree.
   for (size_t i = 0; i < curTx_.getNumTxOut(); i++) {
      const auto outAddr = bs::Address::fromTxOut(curTx_.getTxOutCopy(i));
      double amtBTC = curTx_.getTxOutCopy(i).getValue() / BTCNumericTypes::BalanceDivider;

      // create a top level item using type, address, amount, wallet values
      QTreeWidgetItem *item = createItem(tree,
                                         tr("Output"),
                                         outAddr.display(),
                                         QString::number(amtBTC,
                                                         'f',
                                                         BTCNumericTypes::default_precision),
                                         QString());

      // Example: Add several child items to this top level item to crate a new
      // branch in the tree.
/*      item->addChild(createItem(item,
                                tr("1JSAGsDo56rEqgxf3R1EAiCgwGJCUB31Cr"),
                                tr("1JSAGsDo56rEqgxf3R1EAiCgwGJCUB31Cr"),
                                tr("-0.00850000"),
                                tr("Settlement")));
      item->setExpanded(true);*/

      // add the item to the tree
      tree->addTopLevelItem(item);
   }
   tree->resizeColumns();
}

QTreeWidgetItem * TransactionDetailsWidget::createItem(QTreeWidget *tree,
                                                       QString type,
                                                       QString address,
                                                       QString amount,
                                                       QString wallet)
{
   QTreeWidgetItem *item = new QTreeWidgetItem(tree);
   item->setText(colType, type); // type
   item->setText(colAddressId, address); // address
   item->setText(colAmount, amount); // amount
   item->setText(colWallet, wallet); // wallet
   return item;
}

QTreeWidgetItem * TransactionDetailsWidget::createItem(QTreeWidgetItem *parentItem,
                                                       QString type,
                                                       QString address,
                                                       QString amount,
                                                       QString wallet) {
   QTreeWidgetItem *item = new QTreeWidgetItem(parentItem);
   item->setFirstColumnSpanned(true);
   item->setText(colType, type); // type
   item->setText(colAddressId, address); // address
   item->setText(colAmount, amount); // amount
   item->setText(colWallet, wallet); // wallet
   return item;
}

void TransactionDetailsWidget::onAddressClicked(QTreeWidgetItem *item, int column) {
   // user has clicked the address column of the item so
   // send a signal to ExplorerWidget to open AddressDetailsWidget
   if (column == colAddressId) {
      emit(addressClicked(item->text(colAddressId)));
   }
}

// Clear all the fields.
void TransactionDetailsWidget::clearFields() {
   ui_->tranID->clear();
   ui_->tranDate->clear();
   ui_->tranHeight->clear();
   ui_->tranConfirmations->clear();
   ui_->tranNumInputs->clear();
   ui_->tranNumOutputs->clear();
   ui_->tranOutput->clear();
   ui_->tranFees->clear();
   ui_->tranFeePerByte->clear();
   ui_->tranSize->clear();
   ui_->treeInput->clear();
   ui_->treeOutput->clear();
}
