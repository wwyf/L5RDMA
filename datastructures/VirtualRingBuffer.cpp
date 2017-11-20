#include "VirtualRingBuffer.h"
#include <exchangeableTransports/util/sharedMemory.h>

VirtualRingBuffer::VirtualRingBuffer(size_t size, int sock) : size(size) {
    localRw = malloc_shared<RingBufferInfo>(infoName + pid, sizeof(RingBufferInfo), true);
    local1 = malloc_shared<uint8_t>(bufferName + pid, size, true);
    local2 = malloc_shared<uint8_t>(bufferName + pid, size, false, local1.get() + size);

    domain_write(sock, pid.c_str(), pid.size());
    uint8_t buffer[255];
    size_t readCount = domain_read(sock, buffer, 255);
    const auto remotePid = std::string(buffer, buffer + readCount);

    remoteRw = malloc_shared<RingBufferInfo>(infoName + remotePid, sizeof(RingBufferInfo), false);
    remote1 = malloc_shared<uint8_t>(bufferName + remotePid, size, false);
    remote2 = malloc_shared<uint8_t>(bufferName + remotePid, size, false, remote1.get() + size);
}

void VirtualRingBuffer::send(const uint8_t *data, size_t length) {
    const auto localWritten = localRw->written.load();
    const auto pos = localWritten % size;

    size_t remoteRead;
    do {
        remoteRead = remoteRw->read; // probably buffer this in class, so we don't have as much remote reads
    } while ((localWritten - remoteRead) > (size - length)); // block until there is some space

    std::copy(data, data + length, local1.get() + pos);

    localRw->written += length;
}

size_t VirtualRingBuffer::receive(void *whereTo, size_t maxSize) {
    const auto localRead = localRw->read.load();
    const auto pos = localRead % size;

    size_t remoteWritten;
    do {
        remoteWritten = remoteRw->written; // probably buffer this in class, so we don't have as much remote reads
    } while ((remoteWritten - localRead) < maxSize); // block until maxSize is available

    std::copy(remote1.get() + pos, remote1.get() + pos + maxSize, reinterpret_cast<uint8_t *>(whereTo));

    localRw->read += maxSize;
    return maxSize;
}


