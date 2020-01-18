/*

***********************************************************************************
* Copyright (C) 2016 - 2019, BlockSettle AB
* Distributed under the GNU Affero General Public License (AGPL v3)
* See LICENSE or http://www.gnu.org/licenses/agpl.html
*
**********************************************************************************

*/
#include "UtxoReservation.h"

#include <thread>
#include <spdlog/spdlog.h>

using namespace bs;


// Unreserve all UTXOs for a given reservation ID. Associated wallet ID is the
// return value. Return the associated wallet ID. Adapter acts as a frontend for
// the actual reservation class.
void bs::UtxoReservation::Adapter::unreserve(const std::string &reserveId)
{
   if (!parent_) {
      return;
   }
   parent_->unreserve(reserveId);
}

// Get UTXOs for a given reservation ID. Adapter acts as a frontend for the
// actual reservation class.
std::vector<UTXO> bs::UtxoReservation::Adapter::get(const std::string &reserveId) const
{
   if (!parent_) {
      return {};
   }
   return parent_->get(reserveId);
}

// Reserve UTXOs for given wallet and reservation IDs. True if success, false if
// failure. Adapter acts as a frontend for the actual reservation class.
void bs::UtxoReservation::Adapter::reserve(const std::string &reserveId
   , const std::vector<UTXO> &utxos)
{
   assert(parent_);
   parent_->reserve(reserveId, utxos);
}

// For a given wallet ID, filter out all associated UTXOs from a list of UTXOs.
// True if success, false if failure. Adapter acts as a frontend for the actual
// reservation class.
void bs::UtxoReservation::Adapter::filter(std::vector<UTXO> &utxos) const
{
   assert(parent_);
   parent_->filter(utxos);
}

// Global UTXO reservation singleton.
static std::shared_ptr<bs::UtxoReservation> utxoResInstance_;

bs::UtxoReservation::UtxoReservation(const std::shared_ptr<spdlog::logger> &logger)
   : logger_(logger)
{
}

// Singleton reservation.
void bs::UtxoReservation::init(const std::shared_ptr<spdlog::logger> &logger)
{
   assert(!utxoResInstance_);
   utxoResInstance_ = std::shared_ptr<bs::UtxoReservation>(new bs::UtxoReservation(logger));
}

// Add an adapter to the singleton. True if success, false if failure.
bool bs::UtxoReservation::addAdapter(const std::shared_ptr<Adapter> &a)
{
   if (!utxoResInstance_) {
      return false;
   }
   a->setParent(utxoResInstance_.get());
   std::lock_guard<std::mutex> lock(utxoResInstance_->mutex_);
   utxoResInstance_->adapters_.push_back(a);
   return true;
}

// Remove an adapter from the singleton. True if success, false if failure.
bool bs::UtxoReservation::delAdapter(const std::shared_ptr<Adapter> &a)
{
   if (!utxoResInstance_ || !a) {
      return false;
   }
   std::lock_guard<std::mutex> lock(utxoResInstance_->mutex_);
   const auto pos = std::find(utxoResInstance_->adapters_.begin()
                              , utxoResInstance_->adapters_.end(), a);
   if (pos == utxoResInstance_->adapters_.end()) {
      return false;
   }
   a->setParent(nullptr);
   utxoResInstance_->adapters_.erase(pos);
   return true;
}

// Reserve a set of UTXOs for a wallet and reservation ID. Reserve across all
// active adapters.
void bs::UtxoReservation::reserve(const std::string &reserveId
   , const std::vector<UTXO> &utxos)
{
   const auto curTime = std::chrono::steady_clock::now();
   std::lock_guard<std::mutex> lock(mutex_);

   auto it = byReserveId_.find(reserveId);
   if (it != byReserveId_.end()) {
      SPDLOG_LOGGER_ERROR(logger_, "reservation '{}' already exist", reserveId);
      return;
   }

   byReserveId_[reserveId] = utxos;
   reserveTime_[reserveId] = curTime;

   for (const auto &utxo : utxos) {
      auto result = reserved_.insert(utxo);
      if (!result.second) {
         SPDLOG_LOGGER_ERROR(logger_, "found duplicated reserved UTXO!");
      }
   }

   for (const auto &a : adapters_) {
      a->reserved(utxos);
   }
}

// Unreserve a set of UTXOs for a wallet and reservation ID. Return the
// associated wallet ID. Unreserve across all active adapters.
bool bs::UtxoReservation::unreserve(const std::string &reserveId)
{
   std::lock_guard<std::mutex> lock(mutex_);
   const auto it = byReserveId_.find(reserveId);
   if (it == byReserveId_.end()) {
      return false;
   }

   for (const auto &utxo : it->second) {
      reserved_.erase(utxo);
   }

   byReserveId_.erase(it);

   reserveTime_.erase(reserveId);

   for (const auto &a : adapters_) {
      a->unreserved(reserveId);
   }

   return true;
}

// Get UTXOs for a given reservation ID.
std::vector<UTXO> bs::UtxoReservation::get(const std::string &reserveId) const
{
   std::lock_guard<std::mutex> lock(mutex_);

   const auto it = byReserveId_.find(reserveId);
   if (it == byReserveId_.end()) {
      return {};
   }
   return it->second;
}

// For a given wallet ID, filter out all associated UTXOs from a list of UTXOs.
// True if success, false if failure.
void bs::UtxoReservation::filter(std::vector<UTXO> &utxos) const
{
   std::lock_guard<std::mutex> lock(mutex_);

   auto it = utxos.begin();
   while (it != utxos.end()) {
      if (reserved_.find(*it) != reserved_.end()) {
         it = utxos.erase(it);
      } else {
         ++it;
      }
   }
}

UtxoReservation *UtxoReservation::instance()
{
   return utxoResInstance_.get();
}
