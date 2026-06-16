#include <aquamarine/allocator/Shm.hpp>
#include <aquamarine/backend/Backend.hpp>
#include <aquamarine/allocator/Swapchain.hpp>
#include "FormatUtils.hpp"
#include "Shared.hpp"
#include <cstring>
#include <unistd.h>
#include <sys/mman.h>

using namespace Aquamarine;
using namespace Hyprutils::Memory;
#define SP CSharedPointer
#define WP CWeakPointer

// Bytes per pixel for the (32-bit) formats the WSLg/shm path advertises.
// wl_shm mandates ARGB8888 and XRGB8888; we only allocate those here.
static uint32_t bytesPerPixelForFormat(uint32_t format) {
    switch (format) {
        case DRM_FORMAT_ARGB8888:
        case DRM_FORMAT_XRGB8888:
        case DRM_FORMAT_ABGR8888:
        case DRM_FORMAT_XBGR8888:
        case DRM_FORMAT_ARGB2101010:
        case DRM_FORMAT_XRGB2101010:
        case DRM_FORMAT_ABGR2101010:
        case DRM_FORMAT_XBGR2101010: return 4;
        default: return 0;
    }
}

Aquamarine::CShmBuffer::CShmBuffer(const SAllocatorBufferParams& params, Hyprutils::Memory::CWeakPointer<CShmAllocator> allocator_, Hyprutils::Memory::CSharedPointer<CSwapchain> swapchain) :
    allocator(allocator_) {
    format = params.format;

    const uint32_t bpp = bytesPerPixelForFormat(format);
    if (bpp == 0) {
        allocator->backend->log(AQ_LOG_ERROR, std::format("CShmBuffer: unsupported format {} for a host-memory buffer", fourccToName(format)));
        return;
    }

    pixelSize = {(double)params.size.x, (double)params.size.y};
    stride    = (uint32_t)params.size.x * bpp;
    bufferLen = (size_t)stride * (size_t)params.size.y;
    size      = pixelSize;

    fd = memfd_create("aq-shm-buffer", MFD_CLOEXEC);
    if (fd < 0) {
        allocator->backend->log(AQ_LOG_ERROR, "CShmBuffer: memfd_create failed");
        return;
    }

    if (ftruncate(fd, bufferLen) < 0) {
        allocator->backend->log(AQ_LOG_ERROR, "CShmBuffer: ftruncate failed");
        close(fd);
        fd = -1;
        return;
    }

    data = (uint8_t*)mmap(nullptr, bufferLen, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED) {
        allocator->backend->log(AQ_LOG_ERROR, "CShmBuffer: mmap failed");
        close(fd);
        fd   = -1;
        data = nullptr;
        return;
    }

    // clear so we don't present garbage before the first render
    memset(data, 0x00, bufferLen);

    success = true;

    allocator->backend->log(AQ_LOG_DEBUG,
                            std::format("CShmBuffer: allocated a host-memory buffer fd {}, size {}, stride {}, format {}", fd, pixelSize, stride, fourccToName(format)));
}

Aquamarine::CShmBuffer::~CShmBuffer() {
    events.destroy.emit();

    if (data)
        munmap(data, bufferLen);

    if (fd >= 0)
        close(fd);
}

eBufferCapability Aquamarine::CShmBuffer::caps() {
    return eBufferCapability::BUFFER_CAPABILITY_DATAPTR;
}

eBufferType Aquamarine::CShmBuffer::type() {
    return eBufferType::BUFFER_TYPE_SHM;
}

void Aquamarine::CShmBuffer::update(const Hyprutils::Math::CRegion& damage) {
    ; // nothing to do, updates happen on the cpu mapping directly
}

bool Aquamarine::CShmBuffer::isSynchronous() {
    return true;
}

bool Aquamarine::CShmBuffer::good() {
    return success && data && fd >= 0;
}

SSHMAttrs Aquamarine::CShmBuffer::shm() {
    SSHMAttrs attrs;
    attrs.success = success;
    attrs.fd      = fd;
    attrs.format  = format;
    attrs.size    = pixelSize;
    attrs.stride  = stride;
    attrs.offset  = 0;
    return attrs;
}

std::tuple<uint8_t*, uint32_t, size_t> Aquamarine::CShmBuffer::beginDataPtr(uint32_t flags) {
    return {data, format, bufferLen};
}

void Aquamarine::CShmBuffer::endDataPtr() {
    ; // nothing to do
}

Aquamarine::CShmAllocator::~CShmAllocator() {
    ; // nothing to do
}

SP<CShmAllocator> Aquamarine::CShmAllocator::create(Hyprutils::Memory::CWeakPointer<CBackend> backend_) {
    auto a  = SP<CShmAllocator>(new CShmAllocator(backend_));
    a->self = a;

    backend_->log(AQ_LOG_DEBUG, "CShmAllocator: created a host-memory (shm) allocator");

    return a;
}

SP<IBuffer> Aquamarine::CShmAllocator::acquire(const SAllocatorBufferParams& params, SP<CSwapchain> swapchain_) {
    if (params.size.x < 1 || params.size.y < 1) {
        backend->log(AQ_LOG_ERROR, "CShmAllocator: attempted to allocate a buffer with invalid size");
        return nullptr;
    }

    auto buf = SP<CShmBuffer>(new CShmBuffer(params, self, swapchain_));
    if (!buf->good())
        return nullptr;

    buffers.emplace_back(buf);
    std::erase_if(buffers, [](const auto& b) { return b.expired(); });
    return buf;
}

SP<CBackend> Aquamarine::CShmAllocator::getBackend() {
    return backend.lock();
}

int Aquamarine::CShmAllocator::drmFD() {
    return -1;
}

eAllocatorType Aquamarine::CShmAllocator::type() {
    return eAllocatorType::AQ_ALLOCATOR_TYPE_SHM;
}

Aquamarine::CShmAllocator::CShmAllocator(Hyprutils::Memory::CWeakPointer<CBackend> backend_) : backend(backend_) {
    ; // nothing to do
}
