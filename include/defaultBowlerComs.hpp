/*
 * This file is part of bowler-device-server.
 *
 * bowler-device-server is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * bowler-device-server is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with bowler-device-server.  If not, see <https://www.gnu.org/licenses/>.
 */
#pragma once

#include "bowlerComs.hpp"
#include "bowlerDeviceServerUtil.hpp"
#include "bowlerServer.hpp"
#include "serverManagementPacket.hpp"
#include <map>

namespace bowlerserver {
/**
 * Buffer format is:
 * <ID (1 byte)> <Seq Num (1 byte)> <ACK num (1 byte)> <Payload (N bytes)>.
 */
template <std::size_t N> class DefaultBowlerComs : public BowlerComs<N> {
  // The entire packet length must be at least the header length plus one payload byte
  static_assert(N >= HEADER_LENGTH + 1,
                "Packet length must be at least the header length plus one payload byte.");

  public:
  DefaultBowlerComs(std::unique_ptr<BowlerServer<N>> iserver) : server(std::move(iserver)) {
    // Add the server management packet before anything else gets a chance
    addPacket(std::shared_ptr<ServerManagementPacket<N>>(new ServerManagementPacket<N>(this)));
  }

  virtual ~DefaultBowlerComs() = default;

  void addEnsuredPacket(std::function<std::shared_ptr<Packet>(void)> iaddPacket) override {
    ensuredPackets.push_back(iaddPacket);
  }

  std::int32_t addEnsuredPackets() override {
    for (auto &&elem : ensuredPackets) {
      if (addPacket(elem()) == BOWLER_ERROR) {
        return BOWLER_ERROR;
      }
    }

    return 1;
  }

  /**
   * Adds a packet event handler. The packet id cannot already be used.
   *
   * @param ipacket The packet event handler.
   * @return `1` on success or BOWLER_ERROR on error.
   */
  std::int32_t addPacket(std::shared_ptr<Packet> ipacket) override {
    if (packets.find(ipacket->getId()) == packets.end()) {
      if (ipacket->isReliable()) {
        // Initialize RDT state
        reliableState[ipacket->getId()] = waitForZero;
      }

      // Save the packet last so we can `move` it
      packets[ipacket->getId()] = std::move(ipacket);
    } else {
      // The packet id is already used
      errno = EINVAL;
      return BOWLER_ERROR;
    }

    return 1;
  }

  /**
   * Removes a packet event handler.
   *
   * @param iid The id of the packet.
   */
  void removePacket(const std::uint8_t iid) override {
    packets.erase(iid);
  }

  /**
   * @return Every attached packet id. Does not return the SERVER_MANAGEMENT_PACKET_ID.
   */
  std::vector<std::uint8_t> getAllPacketIDs() override {
    std::vector<std::uint8_t> ids;
    ids.reserve(packets.size() - 1); // Minus 1 for the management packet

    for (auto &&elem : packets) {
      // Don't return the server management packet
      if (elem.first != SERVER_MANAGEMENT_PACKET_ID) {
        ids.push_back(elem.first);
      }
    }

    return ids;
  }

  /**
   * Run an iteration of coms.
   *
   * @return `1` on success or BOWLER_ERROR on error.
   */
  std::int32_t loop() override {
    bool isDataAvailable;
    std::int32_t error = server->isDataAvailable(isDataAvailable);
    if (error != BOWLER_ERROR) {
      if (isDataAvailable) {
        std::array<std::uint8_t, N> data;

        std::int32_t error = server->read(data);
        if (error != BOWLER_ERROR) {
          auto id = getPacketId(data);
          auto packet = packets.find(id);
          if (packet == packets.end()) {
            BOWLER_LOG("Packet with id %u was not found.\n", id);

            // The corresponding packet was not found, meaning there is no handler registered for
            // it. Clear the payload and reply.
            std::fill(std::next(data.begin(), HEADER_LENGTH), data.end(), 0);

            auto writeError = server->write(data);
            if (writeError == BOWLER_ERROR) {
              BOWLER_LOG(
                "Error while replying to unregistered packet: %d %s\n", errno, strerror(errno));
            }

            errno = ENODEV;
            return BOWLER_ERROR;
          } else {
            // The packet handler was found
            if (packet->second->isReliable()) {
              handlePacketReliable(packet, data);
            } else {
              handlePacketUnreliable(packet, data);
            }
          }
        } else {
          // Error reading data
          BOWLER_LOG("Error reading: %d %s\n", errno, strerror(errno));
        }
      }
    } else {
      // Error running isDataAvailable. EWOULDBLOCK is typical of having no data (not really an
      // error).
      if (errno != EWOULDBLOCK) {
        BOWLER_LOG("Error peeking: %d %s\n", errno, strerror(errno));
      }
    }

    return 1;
  }

  protected:
  /**
   * Handles a packet for unreliable transport.
   *
   * @param idata Data that was just read from the receive buffer.
   */
  template <typename T>
  void handlePacketUnreliable(T &ipacket, std::array<std::uint8_t, N> &idata) {
    auto error = ipacket->second->event(idata.data() + HEADER_LENGTH);
    if (error == BOWLER_ERROR) {
      BOWLER_LOG("Error handling packet event: %d %s\n", errno, strerror(errno));
    }

    error = server->write(idata);
    if (error == BOWLER_ERROR) {
      BOWLER_LOG("Error writing: %d %s\n", errno, strerror(errno));
    }
  }

  /**
   * Handles a packet for reliable transport.
   *
   * @param idata Data that was just read from the receive buffer.
   */
  template <typename T> void handlePacketReliable(T &ipacket, std::array<std::uint8_t, N> &idata) {
    states_t &state = reliableState[ipacket->first];
    switch (state) {
    case waitForZero: {
      if (getSeqNum(idata) == 0) {
        // Right payload. Handle it.
        const auto eventError = ipacket->second->event(idata.data() + HEADER_LENGTH);
        if (eventError == BOWLER_ERROR) {
          BOWLER_LOG("Error handling packet event: %d %s\n", errno, strerror(errno));
        }

        // ACK it and start waiting for the next packet.
        setAckNum(idata, 0);
        auto error = server->write(idata);
        if (error == BOWLER_ERROR) {
          BOWLER_LOG("Error writing: %d %s\n", errno, strerror(errno));
        }

        if (ipacket->first == SERVER_MANAGEMENT_PACKET_ID && eventError == 2) {
          // The server management packet processed a disconnection, so force the state into the
          // starting state
          state = waitForZero;
        } else {
          state = waitForOne;
        }
      } else {
        // Wrong packet. Clear the payload and ACK 1.
        std::fill(std::next(idata.begin(), HEADER_LENGTH), idata.end(), 0);
        setAckNum(idata, 1);
        auto error = server->write(idata);
        if (error == BOWLER_ERROR) {
          BOWLER_LOG("Error writing: %d %s\n", errno, strerror(errno));
        }
      }
      break;
    }

    case waitForOne: {
      if (getSeqNum(idata) == 1) {
        // Right payload. Handle it.
        auto error = ipacket->second->event(idata.data() + HEADER_LENGTH);
        if (error == BOWLER_ERROR) {
          BOWLER_LOG("Error handling packet event: %d %s\n", errno, strerror(errno));
        }

        // ACK it and start waiting for the next packet.
        setAckNum(idata, 1);
        error = server->write(idata);
        if (error == BOWLER_ERROR) {
          BOWLER_LOG("Error writing: %d %s\n", errno, strerror(errno));
        }

        // Even if the server management packet processed a disconnection, this returns us to the
        // starting state (which we want)
        state = waitForZero;
      } else {
        // Wrong packet. Clear the payload and ACK 0.
        std::fill(std::next(idata.begin(), HEADER_LENGTH), idata.end(), 0);
        setAckNum(idata, 0);
        auto error = server->write(idata);
        if (error == BOWLER_ERROR) {
          BOWLER_LOG("Error writing: %d %s\n", errno, strerror(errno));
        }
      }
      break;
    }
    }
  }

  std::uint8_t getPacketId(const std::array<std::uint8_t, N> &idata) const {
    return idata.at(0);
  }

  std::uint8_t getSeqNum(const std::array<std::uint8_t, N> &idata) const {
    return idata.at(1);
  }

  std::uint8_t getAckNum(const std::array<std::uint8_t, N> &idata) const {
    return idata.at(2);
  }

  void setSeqNum(std::array<std::uint8_t, N> &idata, std::uint8_t iseqNum) const {
    idata.at(1) = iseqNum;
  }

  void setAckNum(std::array<std::uint8_t, N> &idata, std::uint8_t iackNum) const {
    idata.at(2) = iackNum;
  }

  enum states_t { waitForZero, waitForOne };
  std::unique_ptr<BowlerServer<N>> server;
  std::map<std::uint8_t, std::shared_ptr<Packet>> packets;
  std::map<std::uint8_t, states_t> reliableState;
  std::vector<std::function<std::shared_ptr<Packet>(void)>> ensuredPackets;
};
} // namespace bowlerserver
