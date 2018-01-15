#include "CompletionQueuePair.hpp"
#include "Network.hpp"
#include <cstring>
#include <iostream>
#include <iomanip>

using namespace std;
namespace rdma {
    CompletionQueuePair::CompletionQueuePair(Network &network) :
            channel(network.context->createCompletionEventChannel()), // Create event channel
            // Create completion queues
            sendQueue(network.context->createCompletionQueue(Network::CQ_SIZE, nullptr, *channel, 0)),
            receiveQueue(network.context->createCompletionQueue(Network::CQ_SIZE, nullptr, *channel, 0)) {

        // Request notifications
        sendQueue->requestNotify(false);
        receiveQueue->requestNotify(false);
    }

    CompletionQueuePair::~CompletionQueuePair() {
        for (auto event : eventsToAck) {
            event->ackEvents(1);
        }
    }

    /// Poll a completion queue
    uint64_t CompletionQueuePair::pollCompletionQueue(ibv::completions::CompletionQueue &completionQueue,
                                                      ibv::workcompletion::Opcode type) {
        // Poll for a work completion
        ibv::workcompletion::WorkCompletion completion;
        if (completionQueue.poll(1, &completion) == 0) {
            return numeric_limits<uint64_t>::max();
        }

        // Check status and opcode
        if (completion) {
            if (completion.getOpcode() == type) {
                return completion.getId();
            } else {
                throw NetworkException("unexpected completion opcode: " + to_string(completion.getOpcode()));
            }
        } else {
            throw NetworkException("unexpected completion status: " + to_string(completion.getStatus()));
        }
    }

    /// Wait for a work completion
    pair<bool, uint64_t> CompletionQueuePair::waitForCompletion(bool restricted, bool onlySend) {
        unique_lock<mutex> lock(guard);

        // We have to empty the completion queue and cache additional completions
        // as events are only generated when new work completions are enqueued.

        pair<bool, uint64_t> workCompletion;
        bool found = false;

        for (unsigned c = 0; c != cachedCompletions.size(); ++c) {
            if (!restricted || cachedCompletions[c].first == onlySend) {
                workCompletion = cachedCompletions[c];
                cachedCompletions.erase(cachedCompletions.begin() + c);
                found = true;
                break;
            }
        }

        while (!found) {
            // Wait for completion queue event
            auto[event, ctx] = channel->getEvent();
            std::ignore = ctx;

            event->ackEvents(1);
            bool isSendCompletion = (event == sendQueue.get());

            // Request a completion queue event
            event->requestNotify(false);

            // Poll all work completions
            ibv::workcompletion::WorkCompletion completion;
            int numPolled;
            do {
                numPolled = event->poll(1, &completion);

                if (numPolled == 0) {
                    continue;
                }

                if (not completion.isSuccessful()) {
                    throw NetworkException("unexpected completion status: " + to_string(completion.getStatus()));
                }

                // Add completion
                if (!found && (!restricted || isSendCompletion == onlySend)) {
                    workCompletion = make_pair(isSendCompletion, completion.getId());
                    found = true;
                } else {
                    cachedCompletions.push_back(make_pair(isSendCompletion, completion.getId()));
                }
            } while (numPolled);
        }

        // Return the oldest completion
        return workCompletion;
    }

    /// Poll the send completion queue
    uint64_t CompletionQueuePair::pollSendCompletionQueue() {
        // Poll for a work completion
        ibv::workcompletion::WorkCompletion completion;
        if (sendQueue->poll(1, &completion) == 0) {
            return numeric_limits<uint64_t>::max();
        }

        // Check status and opcode
        if (completion) {
            return completion.getId();
        } else {
            throw NetworkException("unexpected completion status: " + to_string(completion.getStatus()));
        }
    }

    uint64_t CompletionQueuePair::pollSendCompletionQueue(ibv::workcompletion::Opcode type) {
        return pollCompletionQueue(*sendQueue, type);
    }

    /// Poll the receive completion queue
    uint64_t CompletionQueuePair::pollRecvCompletionQueue() {
        return pollCompletionQueue(*receiveQueue, ibv::workcompletion::Opcode::RECV);
    }

    /// Poll a completion queue blocking
    uint64_t
    CompletionQueuePair::pollCompletionQueueBlocking(ibv::completions::CompletionQueue &completionQueue,
                                                     ibv::workcompletion::Opcode type) {
        // Poll for a work completion
        ibv::workcompletion::WorkCompletion completion;
        while (completionQueue.poll(1, &completion) == 0); // busy poll

        // Check status and opcode
        if (completion) {
            if (completion.getOpcode() == type) {
                return completion.getId();
            } else {
                throw NetworkException("unexpected completion opcode: " + to_string(completion.getOpcode()));
            }
        } else {
            throw NetworkException("unexpected completion status: " + to_string(completion.getStatus()));
        }
    }

    /// Poll the send completion queue blocking
    uint64_t CompletionQueuePair::pollSendCompletionQueueBlocking() {
        return pollCompletionQueueBlocking(*sendQueue, ibv::workcompletion::Opcode::RDMA_READ);
    }

    /// Poll the receive completion queue blocking
    uint64_t CompletionQueuePair::pollRecvCompletionQueueBlocking() {
        return pollCompletionQueueBlocking(*receiveQueue, ibv::workcompletion::Opcode::RECV);
    }

    /// Wait for a work completion
    void CompletionQueuePair::waitForCompletion() {
        // Wait for completion queue event
        auto[event, ctx] = channel->getEvent();
        std::ignore = ctx;

        eventsToAck.push_back(event);

        // Request a completion queue event
        event->requestNotify(false);

        // Poll all work completions
        ibv::workcompletion::WorkCompletion completion;
        for (;;) {
            event->poll(1, &completion);
            auto numPolled = event->poll(1, &completion);

            if (numPolled == 0) {
                break;
            }
            if (not completion.isSuccessful()) {
                throw NetworkException("unexpected completion status: " + to_string(completion.getStatus()));
            }
        };

    }

    /// Wait for a work completion
    uint64_t CompletionQueuePair::waitForCompletionSend() {
        return waitForCompletion(true, true).second;
    }

    /// Wait for a work completion
    uint64_t CompletionQueuePair::waitForCompletionReceive() {
        return waitForCompletion(true, false).second;
    }
} // End of namespace rdma
