/*

***********************************************************************************
* Copyright (C) 2016 - 2019, BlockSettle AB
* Distributed under the GNU Affero General Public License (AGPL v3)
* See LICENSE or http://www.gnu.org/licenses/agpl.html
*
**********************************************************************************

*/
#ifndef __UTXO_RESERVATION_H__
#define __UTXO_RESERVATION_H__

#include <atomic>
#include <chrono>
#include <memory>
#include <unordered_set>
#include <unordered_map>
#include "TxClasses.h"

namespace spdlog {
   class logger;
}

namespace bs {
   // A reservation system for UTXOs. It can be fed a list of inputs. The inputs
   // are then set aside and made unavailable for future usage. This is useful
   // for keeping UTXOs from being used, and for accessing UTXOs later (e.g.,
   // when zero conf TXs arrive and the inputs need to be accessed quickly).
   //
   // NB: This is a global singleton that shouldn't be accessed directly. Use an
   // Adapter object to access the singleton and do all the heavy lifting.
   class UtxoReservation
   {
   public:
      // Create the singleton. Use only once!
      // Destroying disabled as it's broken, see BST-2362 for details
      static void init(const std::shared_ptr<spdlog::logger> &logger);

      // Reserve/Unreserve UTXOs. Used as needed. User supplies the wallet ID,
      // a reservation ID, and the UTXOs to reserve.
      void reserve(const std::string &reserveId, const std::vector<UTXO> &utxos);
      bool unreserve(const std::string &reserveId);

      // Get the UTXOs based on the reservation ID.
      std::vector<UTXO> get(const std::string &reserveId) const;

      // Pass in a vector of UTXOs. If any of the UTXOs are in the wallet ID
      // being queried, remove the UTXOs from the vector.
      void filter(std::vector<UTXO> &utxos) const;

      static UtxoReservation *instance();

      explicit UtxoReservation(const std::shared_ptr<spdlog::logger> &logger);

   private:
      using UTXOs = std::vector<UTXO>;
      using IdList = std::unordered_set<std::string>;

      struct UtxoHasher {
         std::size_t operator()(const UTXO &utxo) const {
            return std::hash<std::string>()(utxo.getTxHash().toBinStr());
         }
      };

      mutable std::mutex mutex_;

      // Reservation ID, UTXO vector.
      std::unordered_map<std::string, UTXOs> byReserveId_;

      // Reservation ID, time of reservation
      std::unordered_map<std::string, std::chrono::steady_clock::time_point> reserveTime_;

      std::unordered_set<UTXO, UtxoHasher> reserved_;

      std::shared_ptr<spdlog::logger> logger_;
   };

}  //namespace bs

#endif //__UTXO_RESERVATION_H__
