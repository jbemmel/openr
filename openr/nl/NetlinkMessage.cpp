/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <thread>
#include <vector>

#include <fbzmq/async/StopEventLoopSignalHandler.h>
#include <folly/system/ThreadName.h>

#include "openr/nl/NetlinkRoute.h"
#include "openr/nl/NetlinkSocket.h"

namespace openr {
namespace Netlink {

uint32_t gSequenceNumber{0};

NetlinkMessage::NetlinkMessage()
    : msghdr(reinterpret_cast<struct nlmsghdr*>(msg.data())),
      promise_(std::make_unique<folly::Promise<int>>()) {}

NetlinkMessage::NetlinkMessage(int type)
    : msghdr(reinterpret_cast<struct nlmsghdr*>(msg.data())),
      promise_(std::make_unique<folly::Promise<int>>()) {
  // initialize netlink header
  msghdr->nlmsg_len = NLMSG_LENGTH(0);
  msghdr->nlmsg_type = type;
}

struct nlmsghdr*
NetlinkMessage::getMessagePtr() {
  return msghdr;
}

void
NetlinkMessage::updateBytesReceived(uint16_t bytes) {
  size_ = bytes;
}

uint32_t
NetlinkMessage::getDataLength() const {
  return msghdr->nlmsg_len;
}

struct rtattr*
NetlinkMessage::addSubAttributes(
    struct rtattr* rta, int type, const void* data, uint32_t len) const {
  uint32_t subRtaLen = RTA_LENGTH(len);

  if (RTA_ALIGN(rta->rta_len) + RTA_ALIGN(subRtaLen) > size_) {
    VLOG(1) << "Space not available to add sub attribute type " << type;
    return nullptr;
  }

  VLOG(2) << "Sub attribute type : " << type << " Len: " << len;

  // add the subattribute
  struct rtattr* subrta =
      (struct rtattr*)(((char*)rta) + RTA_ALIGN(rta->rta_len));
  subrta->rta_type = type;
  subrta->rta_len = subRtaLen;
  if (data) {
    memcpy(RTA_DATA(subrta), data, len);
  }

  // update the RTA length
  rta->rta_len = NLMSG_ALIGN(rta->rta_len) + RTA_ALIGN(subRtaLen);
  return subrta;
}

ResultCode
NetlinkMessage::addAttributes(
    int type,
    const char* const data,
    uint32_t len,
    struct nlmsghdr* const msghdr) {
  uint32_t rtaLen = (RTA_LENGTH(len));
  uint32_t nlmsgAlen = NLMSG_ALIGN((msghdr)->nlmsg_len);

  if (nlmsgAlen + RTA_ALIGN(rtaLen) > size_) {
    VLOG(1) << "Space not available to add attribute type " << type;
    return ResultCode::NO_MESSAGE_BUFFER;
  }

  // set the pointer to the aligned location
  struct rtattr* rptr =
      reinterpret_cast<struct rtattr*>(((char*)(msghdr)) + nlmsgAlen);
  rptr->rta_type = type;
  rptr->rta_len = rtaLen;
  VLOG(2) << "Attribute type : " << type << " Len: " << rtaLen;
  if (data) {
    memcpy(RTA_DATA(rptr), data, len);
  }

  // update the length in NL MSG header
  msghdr->nlmsg_len = nlmsgAlen + RTA_ALIGN(rtaLen);
  return ResultCode::SUCCESS;
}

folly::Future<int>
NetlinkMessage::getFuture() {
  return promise_->getFuture();
}

void
NetlinkMessage::setReturnStatus(int status) {
  promise_->setValue(status);
}

NetlinkProtocolSocket::NetlinkProtocolSocket(fbzmq::ZmqEventLoop* evl)
    : evl_(evl) {
  nlMessageTimer_ = fbzmq::ZmqTimeout::make(evl_, [this]() noexcept {
    LOG(INFO) << "Did not receive last ack " << lastSeqNo_;
    sendNetlinkMessage();
  });
}

void
NetlinkProtocolSocket::init() {
  pid_ = static_cast<int>(
      std::hash<std::thread::id>{}(std::this_thread::get_id()));

  nlSock_ = ::socket(PF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
  if (nlSock_ < 0) {
    LOG(FATAL) << "Netlink socket create failed.";
  }
  VLOG(1) << "Netlink socket created." << nlSock_;
  int size = kNetlinkSockRecvBuf;
  // increase socket recv buffer size
  if (setsockopt(nlSock_, SOL_SOCKET, SO_RCVBUF, &size, sizeof(size)) < 0) {
    LOG(FATAL) << "Netlink socket set recv buffer failed.";
  };

  // set the source address
  ::memset(&saddr_, 0, sizeof(saddr_));
  saddr_.nl_family = AF_NETLINK;
  saddr_.nl_pid = pid_;
  saddr_.nl_groups =
      RTMGRP_IPV6_ROUTE | RTMGRP_LINK | RTMGRP_IPV4_IFADDR | RTMGRP_IPV6_IFADDR;

  if (bind(nlSock_, (struct sockaddr*)&saddr_, sizeof(saddr_)) != 0) {
    LOG(FATAL) << "Failed to bind netlink socket: " << folly::errnoStr(errno);
  };

  evl_->addSocketFd(nlSock_, ZMQ_POLLIN, [this](int) noexcept {
    try {
      recvNetlinkMessage();
    } catch (std::exception const& err) {
      LOG(ERROR) << "error processing NL message" << folly::exceptionStr(err);
      ++errors_;
    }
  });
}

void
NetlinkProtocolSocket::setLinkEventCB(
    std::function<void(fbnl::Link, int, bool)> linkEventCB) {
  linkEventCB_ = linkEventCB;
}

void
NetlinkProtocolSocket::setAddrEventCB(
    std::function<void(fbnl::IfAddress, int, bool)> addrEventCB) {
  addrEventCB_ = addrEventCB;
}

void
NetlinkProtocolSocket::processAck(uint32_t ack) {
  if (ack == lastSeqNo_) {
    VLOG(2) << "Last ack received " << ack;
    // cancel active message timer
    if (nlMessageTimer_->isScheduled()) {
      nlMessageTimer_->cancelTimeout();
    }
    // continue sending next set of messages
    sendNetlinkMessage();
  }
}

void
NetlinkProtocolSocket::sendNetlinkMessage() {
  evl_->runImmediatelyOrInEventLoop([this]() {
    struct sockaddr_nl nladdr = {
        .nl_family = AF_NETLINK, .nl_pad = 0, .nl_pid = 0, .nl_groups = 0};
    uint32_t count{0};
    uint32_t iovSize = std::min(msgQueue_.size(), kMaxIovMsg);

    if (!iovSize) {
      return;
    }

    auto iov = std::make_unique<struct iovec[]>(iovSize);

    while (count < iovSize && !msgQueue_.empty()) {
      auto m = std::move(msgQueue_.front());
      msgQueue_.pop();

      struct nlmsghdr* nlmsg_hdr = m->getMessagePtr();
      iov[count].iov_base = reinterpret_cast<void*>(m->getMessagePtr());
      iov[count].iov_len = m->getDataLength();

      // fill sequence number and PID
      nlmsg_hdr->nlmsg_seq = ++gSequenceNumber;
      nlmsg_hdr->nlmsg_pid = pid_;

      // check if one request per message
      if ((nlmsg_hdr->nlmsg_flags & NLM_F_MULTI) != 0) {
        LOG(ERROR) << "Error: multipart netlink message not supported";
      }

      // Add seq number -> netlink request mapping
      nlSeqNoMap_.insert({gSequenceNumber, std::move(m)});
      count++;
    }
    lastSeqNo_ = gSequenceNumber;
    VLOG(2) << "Last seq sent:" << lastSeqNo_;

    auto outMsg = std::make_unique<struct msghdr>();
    outMsg->msg_name = &nladdr;
    outMsg->msg_namelen = sizeof(nladdr);
    outMsg->msg_iov = &iov[0];
    outMsg->msg_iovlen = count;

    VLOG(2) << "Sending " << outMsg->msg_iovlen << " netlink messages";
    auto status = sendmsg(nlSock_, outMsg.get(), 0);

    if (status < 0) {
      LOG(ERROR) << "Error sending on NL socket "
                 << folly::errnoStr(std::abs(status))
                 << " Number of messages:" << outMsg->msg_iovlen;
      ++errors_;
    }

    // Schedule timer to wait for acks and send next set of messages
    nlMessageTimer_->scheduleTimeout(
        std::chrono::milliseconds(kNlMessageAckTimer));
  });
}

void
NetlinkProtocolSocket::setReturnStatusValue(uint32_t seq, int status) {
  try {
    auto request = nlSeqNoMap_.at(seq);
    request->setReturnStatus(status);
    // Remove mapping
    nlSeqNoMap_.erase(seq);
  } catch (const std::out_of_range& e) {
    VLOG(2) << "No future associated with Seq#" << seq;
  }
}

void
NetlinkProtocolSocket::processMessage(
    const std::array<char, kMaxNlPayloadSize>& rxMsg, uint32_t bytesRead) {
  // first netlink message header
  struct nlmsghdr* nlh = (struct nlmsghdr*)rxMsg.data();
  do {
    if (!NLMSG_OK(nlh, bytesRead)) {
      break;
    }

    VLOG(2) << "Netlink message of type " << nlh->nlmsg_type;
    switch (nlh->nlmsg_type) {
    case RTM_NEWROUTE:
    case RTM_DELROUTE: {
      // next RTM message to be processed
      auto rtmMessage = std::make_unique<NetlinkRouteMessage>();
    } break;

    case RTM_DELLINK:
    case RTM_NEWLINK: {
      // process link information received from netlink
      auto linkMessage = std::make_unique<NetlinkLinkMessage>();
      fbnl::Link link = linkMessage->parseMessage(nlh);
      if (nlSeqNoMap_.count(nlh->nlmsg_seq) > 0) {
        // Synchronous event - do not generate link events
        auto& linkAttr = links_[link.getLinkName()];
        linkAttr.isUp = link.isUp();
        linkAttr.ifIndex = link.getIfIndex();
      } else if (linkEventCB_) {
        // Asynchronous event - generate link event for handler
        LOG(INFO) << "Asynchronous Link Event for Link:" << link.getLinkName();
        linkEventCB_(
            link,
            nlh->nlmsg_type == RTM_NEWLINK ? NL_ACT_GET : NL_ACT_DEL,
            true);
      }
    } break;

    case RTM_DELADDR:
    case RTM_NEWADDR: {
      // process interface address information received from netlink
      auto addrMessage = std::make_unique<NetlinkAddrMessage>();
      fbnl::IfAddress addr = addrMessage->parseMessage(nlh);
      if (!addr.getPrefix().hasValue()) {
        break;
      }
      // Valid address, add to link attributes
      if (nlSeqNoMap_.count(nlh->nlmsg_seq) > 0) {
        // Synchronous event - do not generate addr events
        for (auto& linkEntry : links_) {
          if (linkEntry.second.ifIndex == addr.getIfIndex()) {
            linkEntry.second.networks.insert(addr.getPrefix().value());
          }
        }
      } else if (addrEventCB_) {
        // Asynchronous event - generate addr event for handler
        LOG(INFO) << "Asynchronous Addr Event for Prefix:"
                  << addr.getPrefix().value().first
                  << " intf-index:" << addr.getIfIndex();
        addrEventCB_(
            addr,
            nlh->nlmsg_type == RTM_NEWADDR ? NL_ACT_GET : NL_ACT_DEL,
            true);
      }
    } break;

    case NLMSG_ERROR: {
      const struct nlmsgerr* const ack =
          reinterpret_cast<struct nlmsgerr*>(NLMSG_DATA(nlh));
      if (ack->msg.nlmsg_pid != pid_) {
        break;
      }
      setReturnStatusValue(ack->msg.nlmsg_seq, ack->error);
      if (std::abs(ack->error) != EEXIST && std::abs(ack->error) != 0) {
        ++errors_;
      }
      if (ack->error == 0) {
        ++acks_;
      }
      processAck(ack->msg.nlmsg_seq);
    } break;

    case NLMSG_NOOP:
      break;

    case NLMSG_DONE: {
      // End of multipart message
      processAck(nlh->nlmsg_seq);
      setReturnStatusValue(nlh->nlmsg_seq, 0);
    } break;

    default:
      LOG(ERROR) << "Unknown message type: " << nlh->nlmsg_type;
      ++errors_;
    }
  } while ((nlh = NLMSG_NEXT(nlh, bytesRead)));
}

void
NetlinkProtocolSocket::recvNetlinkMessage() {
  // messages buffer
  std::array<char, kMaxNlPayloadSize> recvMsg = {};

  int32_t bytesRead = ::recv(nlSock_, recvMsg.data(), kMaxNlPayloadSize, 0);
  VLOG(4) << "Message received with size: " << bytesRead;

  if (bytesRead < 0) {
    if (errno == EINTR || errno == EAGAIN) {
      return;
    }
    LOG(INFO) << "Error in netlink socket receive: " << bytesRead
              << " err: " << folly::errnoStr(std::abs(errno));
    return;
  }
  processMessage(recvMsg, static_cast<uint32_t>(bytesRead));
}

uint32_t
NetlinkProtocolSocket::getErrorCount() const {
  return errors_;
}

uint32_t
NetlinkProtocolSocket::getAckCount() const {
  return acks_;
}

NetlinkProtocolSocket::~NetlinkProtocolSocket() {
  LOG(INFO) << "Closing netlink socket.";
  close(nlSock_);
}

void
NetlinkProtocolSocket::addNetlinkMessage(
    std::vector<std::unique_ptr<NetlinkMessage>> nlmsgs) {
  evl_->runImmediatelyOrInEventLoop(
      [this, nlmsgs = std::move(nlmsgs)]() mutable {
        auto queueSize = msgQueue_.size();
        for (auto& nlmsg : nlmsgs) {
          if (++queueSize < kMaxNlMessageQueue) {
            msgQueue_.push(std::move(nlmsg));
          } else {
            LOG(ERROR) << "Limit of" << queueSize
                       << " for pending netlink messages reached, discarding";
            break;
          }
        }
        // call send messages API if no timers are scheduled
        if (!nlMessageTimer_->isScheduled()) {
          sendNetlinkMessage();
        }
      });
  return;
}

ResultCode
NetlinkProtocolSocket::getReturnStatus(
    std::vector<folly::Future<int>>& futures,
    std::unordered_set<int> ignoredErrors,
    std::chrono::milliseconds timeout) {
  if (!futures.size()) {
    // No request messages
    return ResultCode::SUCCESS;
  }

  // Collect request status(es) from the request message futures
  auto all = collectAll(futures.begin(), futures.end());
  // Wait for Netlink Ack (which sets the promise value)
  if (all.wait(timeout).isReady()) {
    // Collect statuses from individual futures
    for (const auto& future : futures) {
      if (std::abs(future.value()) != 0 &&
          ignoredErrors.count(std::abs(future.value())) == 0) {
        // Not one of the ignored errors, log
        LOG(ERROR) << "One or more Netlink requests failed with error code:"
                   << std::abs(future.value()) << " -- "
                   << folly::errnoStr(std::abs(future.value()));
        return ResultCode::SYSERR;
      }
    }
    return ResultCode::SUCCESS;
  } else {
    // Atleast one request was timed out.
    LOG(ERROR) << "One or more Netlink requests timed out";
    return ResultCode::TIMEOUT;
  }
}

ResultCode
NetlinkProtocolSocket::addRoute(const openr::fbnl::Route& route) {
  auto rtmMsg = std::make_unique<openr::Netlink::NetlinkRouteMessage>();
  std::vector<folly::Future<int>> futures;
  futures.emplace_back(rtmMsg->getFuture());
  ResultCode status{ResultCode::SUCCESS};
  if ((status = rtmMsg->addRoute(route)) != ResultCode::SUCCESS) {
    LOG(ERROR) << "Error adding route " << route.str();
    return status;
  };
  std::vector<std::unique_ptr<NetlinkMessage>> msg;
  msg.emplace_back(std::move(rtmMsg));
  addNetlinkMessage(std::move(msg));
  return getReturnStatus(futures, std::unordered_set<int>{EEXIST});
}

ResultCode
NetlinkProtocolSocket::addRoutes(const std::vector<openr::fbnl::Route> routes) {
  std::vector<std::unique_ptr<NetlinkMessage>> msg;
  std::vector<folly::Future<int>> futures;

  for (const auto& route : routes) {
    auto rtmMsg = std::make_unique<openr::Netlink::NetlinkRouteMessage>();
    ResultCode status{ResultCode::SUCCESS};
    if (route.getFamily() == AF_MPLS) {
      status = rtmMsg->addLabelRoute(route);
    } else {
      status = rtmMsg->addRoute(route);
    }
    if (status == ResultCode::SUCCESS) {
      futures.emplace_back(rtmMsg->getFuture());
      msg.emplace_back(std::move(rtmMsg));
    } else {
      LOG(ERROR) << "Error adding route " << route.str();
    }
  }
  if (msg.size()) {
    addNetlinkMessage(std::move(msg));
  }
  return getReturnStatus(
      futures, std::unordered_set<int>{EEXIST}, kNlRequestTimeout);
}

ResultCode
NetlinkProtocolSocket::deleteRoute(const openr::fbnl::Route& route) {
  auto rtmMsg = std::make_unique<openr::Netlink::NetlinkRouteMessage>();
  std::vector<folly::Future<int>> futures;
  futures.emplace_back(rtmMsg->getFuture());
  ResultCode status{ResultCode::SUCCESS};
  if ((status = rtmMsg->deleteRoute(route)) != ResultCode::SUCCESS) {
    LOG(ERROR) << "Error deleting route " << route.str();
    return status;
  };
  std::vector<std::unique_ptr<NetlinkMessage>> msg;
  msg.emplace_back(std::move(rtmMsg));
  addNetlinkMessage(std::move(msg));
  // Ignore EEXIST, ESRCH, EINVAL errors in delete operation
  return getReturnStatus(
      futures, std::unordered_set<int>{EEXIST, ESRCH, EINVAL});
}

ResultCode
NetlinkProtocolSocket::addLabelRoute(const openr::fbnl::Route& route) {
  auto rtmMsg = std::make_unique<openr::Netlink::NetlinkRouteMessage>();
  std::vector<folly::Future<int>> futures;
  futures.emplace_back(rtmMsg->getFuture());
  ResultCode status{ResultCode::SUCCESS};
  if ((status = rtmMsg->addLabelRoute(route)) != ResultCode::SUCCESS) {
    LOG(ERROR) << "Error adding label route " << route.str();
    return status;
  };
  std::vector<std::unique_ptr<NetlinkMessage>> msg;
  msg.emplace_back(std::move(rtmMsg));
  addNetlinkMessage(std::move(msg));
  return getReturnStatus(futures, std::unordered_set<int>{EEXIST});
}

ResultCode
NetlinkProtocolSocket::deleteLabelRoute(const openr::fbnl::Route& route) {
  auto rtmMsg = std::make_unique<openr::Netlink::NetlinkRouteMessage>();
  std::vector<folly::Future<int>> futures;
  futures.emplace_back(rtmMsg->getFuture());
  ResultCode status{ResultCode::SUCCESS};
  if ((status = rtmMsg->deleteLabelRoute(route)) != ResultCode::SUCCESS) {
    LOG(ERROR) << "Error deleting label route " << route.str();
    return status;
  };
  std::vector<std::unique_ptr<NetlinkMessage>> msg;
  msg.emplace_back(std::move(rtmMsg));
  addNetlinkMessage(std::move(msg));
  // Ignore EEXIST, ESRCH, EINVAL errors in delete operation
  return getReturnStatus(
      futures, std::unordered_set<int>{EEXIST, ESRCH, EINVAL});
}

ResultCode
NetlinkProtocolSocket::deleteRoutes(
    const std::vector<openr::fbnl::Route> routes) {
  std::vector<std::unique_ptr<NetlinkMessage>> msg;
  std::vector<folly::Future<int>> futures;

  for (const auto& route : routes) {
    auto rtmMsg = std::make_unique<openr::Netlink::NetlinkRouteMessage>();
    ResultCode status{ResultCode::SUCCESS};
    if (route.getFamily() == AF_MPLS) {
      status = rtmMsg->deleteLabelRoute(route);
    } else {
      status = rtmMsg->deleteRoute(route);
    }
    if (status == ResultCode::SUCCESS) {
      futures.emplace_back(rtmMsg->getFuture());
      msg.emplace_back(std::move(rtmMsg));
    } else {
      LOG(ERROR) << "Error deleting route " << route.str();
    }
  }
  if (msg.size()) {
    addNetlinkMessage(std::move(msg));
  }
  // Ignore EEXIST, ESRCH, EINVAL errors in delete operation
  return getReturnStatus(
      futures,
      std::unordered_set<int>{EEXIST, ESRCH, EINVAL},
      kNlRequestTimeout);
}

fbnl::NlLinks
NetlinkProtocolSocket::getAllLinks() {
  // Refresh internal cache
  links_.clear();
  // Send Netlink message to get links
  auto linkMsg = std::make_unique<openr::Netlink::NetlinkLinkMessage>();
  std::vector<folly::Future<int>> futures;
  futures.emplace_back(linkMsg->getFuture());
  linkMsg->init(RTM_GETLINK, 0);
  std::vector<std::unique_ptr<NetlinkMessage>> msg;
  msg.emplace_back(std::move(linkMsg));
  addNetlinkMessage(std::move(msg));
  getReturnStatus(futures, std::unordered_set<int>{});
  // Now get interface addresses from Netlink
  getAllIfAddresses();
  return links_;
}

void
NetlinkProtocolSocket::getAllIfAddresses() {
  auto addrMsg = std::make_unique<openr::Netlink::NetlinkAddrMessage>();
  std::vector<folly::Future<int>> futures;
  futures.emplace_back(addrMsg->getFuture());
  addrMsg->init(RTM_GETADDR, 0);
  std::vector<std::unique_ptr<NetlinkMessage>> msg;
  msg.emplace_back(std::move(addrMsg));
  addNetlinkMessage(std::move(msg));
  getReturnStatus(futures, std::unordered_set<int>{});
}

} // namespace Netlink
} // namespace openr
