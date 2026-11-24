// Console Xbox CreateDescriptorHeap (issue #313 comment 7107265058) passes a 16-byte PC
// D3D12_DESCRIPTOR_HEAP_DESC followed by address at +0x10 and size at +0x18. The title asserts
// GetCPUDescriptorHandleForHeapStart() == address. This tests the explicit Leonida console-heap
// creation path used by the Xbox device proxy slot 14, and (CL-02) that a shader-visible one gets an arena
// shadow that table VAs inside the heap are moved into. The GPU reads through that shadow are in
// d3d12_compute_test ("console-addressed heap").
#include "vk_test_device.h"
#include "../shims/d3d12_com.h"
#include "../shims/d3d12_xs_device.h"

#include <cstdint>
#include <cstdio>

namespace {

template <class T>
struct Ref {
    T* p = nullptr;
    ~Ref() { if (p) p->Release(); }
    T** put() {
        if (p) {
            p->Release();
            p = nullptr;
        }
        return &p;
    }
    T* get() const { return p; }
    T* operator->() const { return p; }  // BUILD-1: the checks below call through Ref (C2819 without it)
};

bool check_console_addressed_heap(ID3D12Device* device) {
    D3D12_DESCRIPTOR_HEAP_DESC desc{};
    desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    desc.NumDescriptors = 8;
    desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    desc.NodeMask = 0;
    const D3D12_GPU_VIRTUAL_ADDRESS address = 0x112233440000ull;
    const std::uint64_t size = 8ull * 32ull;

    Ref<ID3D12DescriptorHeap> heap;
    const HRESULT hr = rt::d3d12::create_console_descriptor_heap(
        device, &desc, address, size, IID_PPV_ARGS(heap.put()));
    if (FAILED(hr) || !heap.get()) {
        std::fprintf(stderr, "create_console_descriptor_heap failed: 0x%08lx\n", static_cast<unsigned long>(hr));
        return false;
    }
    const auto cpu = heap->GetCPUDescriptorHandleForHeapStart();
    if (cpu.ptr != static_cast<SIZE_T>(address)) {
        std::fprintf(stderr, "CPU heap start mismatch: got 0x%llx want 0x%llx\n",
                     static_cast<unsigned long long>(cpu.ptr), static_cast<unsigned long long>(address));
        return false;
    }
    const auto gpu = heap->GetGPUDescriptorHandleForHeapStart();
    if (gpu.ptr != address) {
        std::fprintf(stderr, "GPU heap start mismatch: got 0x%llx want 0x%llx\n",
                     static_cast<unsigned long long>(gpu.ptr), static_cast<unsigned long long>(address));
        return false;
    }
    const D3D12_DESCRIPTOR_HEAP_DESC got = heap->GetDesc();
    if (got.Type != desc.Type || got.NumDescriptors != desc.NumDescriptors || got.Flags != desc.Flags ||
        got.NodeMask != 1) {
        std::fprintf(stderr, "GetDesc mismatch\n");
        return false;
    }
    // CL-02: the shader-visible heap owns an arena range, below 4 GiB, that is not the title's address. Table VAs in
    // [address, address + size) move into it by the same offset; VAs outside it, and other devices' VAs, do not.
    const D3D12_GPU_VIRTUAL_ADDRESS shadow = rt::d3d12::console_heap_shadow_base(heap.get());
    if (!shadow || shadow >= (1ull << 32) || shadow == address) {
        std::fprintf(stderr, "console heap has no arena shadow: 0x%llx\n", static_cast<unsigned long long>(shadow));
        return false;
    }
    IUnknown* const owner = device;
    const struct {
        D3D12_GPU_VIRTUAL_ADDRESS va, want;
    } cases[] = {
        {address, shadow},
        {address + 3 * 32, shadow + 3 * 32},
        {address + size - 1, shadow + size - 1},
        {address + size, address + size},  // one past the heap: not translated
        {address - 32, address - 32},
        {0x1000, 0x1000},
    };
    for (const auto& c : cases) {
        const D3D12_GPU_VIRTUAL_ADDRESS got_va = rt::d3d12::console_heap_shader_address(owner, c.va);
        if (got_va != c.want) {
            std::fprintf(stderr, "translate 0x%llx: got 0x%llx want 0x%llx\n", static_cast<unsigned long long>(c.va),
                         static_cast<unsigned long long>(got_va), static_cast<unsigned long long>(c.want));
            return false;
        }
    }
    int other = 0;
    if (rt::d3d12::console_heap_shader_address(reinterpret_cast<IUnknown*>(&other), address) != address) {
        std::fprintf(stderr, "another device's VA was translated\n");
        return false;
    }
    // A non-shader-visible console heap is CPU-only: no shadow.
    D3D12_DESCRIPTOR_HEAP_DESC staging = desc;
    staging.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    Ref<ID3D12DescriptorHeap> cpu_only;
    if (FAILED(rt::d3d12::create_console_descriptor_heap(device, &staging, address + 0x100000, size,
                                                         IID_PPV_ARGS(cpu_only.put()))) ||
        rt::d3d12::console_heap_shadow_base(cpu_only.get()) != 0) {
        std::fprintf(stderr, "non-shader-visible console heap got a shadow\n");
        return false;
    }
    // A second shader-visible heap of the same device, not overlapping: its own shadow, and each range answers for
    // itself (the registry holds both; Codex review, event 24).
    {
        const D3D12_GPU_VIRTUAL_ADDRESS address2 = address + 0x200000;
        Ref<ID3D12DescriptorHeap> second;
        if (FAILED(rt::d3d12::create_console_descriptor_heap(device, &desc, address2, size, IID_PPV_ARGS(second.put())))) {
            std::fprintf(stderr, "second console heap refused\n");
            return false;
        }
        const D3D12_GPU_VIRTUAL_ADDRESS shadow2 = rt::d3d12::console_heap_shadow_base(second.get());
        if (!shadow2 || shadow2 == shadow ||
            rt::d3d12::console_heap_shader_address(owner, address2 + 32) != shadow2 + 32 ||
            rt::d3d12::console_heap_shader_address(owner, address + 32) != shadow + 32) {
            std::fprintf(stderr, "two console heaps: wrong shadow or translation\n");
            return false;
        }
    }
    // Released, the heap's range translates no more.
    const std::uint64_t before = rt::d3d12::console_heap_stats().heaps;
    heap.put();
    if (rt::d3d12::console_heap_shader_address(owner, address) != address ||
        rt::d3d12::console_heap_stats().heaps + 1 != before) {
        std::fprintf(stderr, "released console heap still translates\n");
        return false;
    }
    return true;
}

bool check_xs_proxy_slot14(ID3D12Device* real_device) {
    void* proxy_raw = nullptr;
    // create_device_proxy takes one reference to the device on success (d3d12_xs_device.h); main's Ref keeps its own,
    // so hand the proxy a new one (BUILD-1: the base test did not, and the device was released twice at exit).
    real_device->AddRef();
    // BUILD-1: IID_PPV_ARGS on a void** names no interface (C2100, C2660); the proxy is an ID3D12Device.
    if (FAILED(rt::d3d12_xs::create_device_proxy(real_device, __uuidof(ID3D12Device), &proxy_raw)) || !proxy_raw) {
        real_device->Release();  // on failure the caller keeps its reference
        std::fprintf(stderr, "create_device_proxy failed\n");
        return false;
    }
    Ref<ID3D12Device> proxy;
    proxy.p = static_cast<ID3D12Device*>(proxy_raw);

    struct ConsoleDescriptorHeapDesc {
        D3D12_DESCRIPTOR_HEAP_DESC desc{};
        std::uint64_t address = 0;
        std::uint64_t size = 0;
    } console{};
    // The title passes memory it owns and writes (BUILD-2: the proxy now takes the extended words only when they
    // describe such memory, so an unmapped made-up address is a PC description).
    void* const title = VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!title) {
        std::fprintf(stderr, "VirtualAlloc failed\n");
        return false;
    }
    console.desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    console.desc.NumDescriptors = 4;
    console.desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    console.address = reinterpret_cast<std::uintptr_t>(title);
    console.size = 4ull * 32ull;

    bool ok = true;
    {
        Ref<ID3D12DescriptorHeap> heap;
        const HRESULT hr = proxy->CreateDescriptorHeap(&console.desc, IID_PPV_ARGS(heap.put()));
        if (FAILED(hr) || !heap.get()) {
            std::fprintf(stderr, "proxy CreateDescriptorHeap(console) failed: 0x%08lx\n",
                         static_cast<unsigned long>(hr));
            ok = false;
        } else {
            // A heap proxy answers slots 9/10 in RAX (the Xbox ABI), not through a hidden pointer as the PC call
            // declared on ID3D12DescriptorHeap does (BUILD-1: the base test called it the PC way and read garbage).
            using XboxHeapHandleFn = std::uint64_t(WINAPI*)(void* heap);
            void* const raw = heap.get();
            const auto slot9 = reinterpret_cast<XboxHeapHandleFn>((*reinterpret_cast<void***>(raw))[9]);
            const std::uint64_t cpu = slot9(raw);
            if (cpu != console.address) {
                std::fprintf(stderr, "proxy CPU heap start mismatch: got 0x%llx want 0x%llx\n",
                             static_cast<unsigned long long>(cpu), static_cast<unsigned long long>(console.address));
                ok = false;
            }
        }
    }
    // A PC 16-byte description through the proxy whose next words do not describe writable memory is a normal heap.
    console.address = 0x556677880000ull;  // not mapped in this process
    {
        Ref<ID3D12DescriptorHeap> pc;
        if (FAILED(proxy->CreateDescriptorHeap(&console.desc, IID_PPV_ARGS(pc.put()))) || !pc.get()) {
            std::fprintf(stderr, "proxy CreateDescriptorHeap(unmapped extended words) failed\n");
            ok = false;
        } else {
            using XboxHeapHandleFn = std::uint64_t(WINAPI*)(void* heap);
            void* const raw = pc.get();
            const auto slot9 = reinterpret_cast<XboxHeapHandleFn>((*reinterpret_cast<void***>(raw))[9]);
            if (slot9(raw) == console.address) {
                std::fprintf(stderr, "unmapped extended words were taken for a console heap\n");
                ok = false;
            }
        }
    }
    VirtualFree(title, 0, MEM_RELEASE);
    return ok;
}

bool check_normal_heap_unchanged(ID3D12Device* device) {
    D3D12_DESCRIPTOR_HEAP_DESC desc{};
    desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    desc.NumDescriptors = 4;
    desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    Ref<ID3D12DescriptorHeap> heap;
    const HRESULT hr = device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(heap.put()));
    if (FAILED(hr) || !heap.get()) {
        std::fprintf(stderr, "CreateDescriptorHeap(normal) failed: 0x%08lx\n", static_cast<unsigned long>(hr));
        return false;
    }
    const auto cpu = heap->GetCPUDescriptorHandleForHeapStart();
    if (cpu.ptr == 0) {
        std::fprintf(stderr, "normal heap CPU start is null\n");
        return false;
    }
    return true;
}

}  // namespace

int main() {
    vktest::Device vk;
    vk.request_buffer_device_address = true;  // the GPU address arena, which holds the CL-02 shadows, needs it
    const auto status = vk.initialize();
    if (status == vktest::DeviceStatus::unavailable) return 77;
    if (status != vktest::DeviceStatus::ready) return 1;

    Ref<ID3D12Device> device;
    rt::d3d12::VulkanDeviceInfo info{};
    info.get_instance_proc = vk.instance_proc;
    info.instance = vk.instance;
    info.physical = vk.physical;
    info.device = vk.device;
    info.queue = vk.queue;
    info.queue_family = vk.family;
    info.buffer_device_address = vk.buffer_device_address;
    if (!info.buffer_device_address) {
        std::fprintf(stderr, "bufferDeviceAddress not enabled: no arena, so no console-heap shadow to check\n");
        return 1;
    }
    if (FAILED(rt::d3d12::create_vulkan_device(info, IID_PPV_ARGS(device.put()))) || !device.get()) {
        std::fprintf(stderr, "create_vulkan_device failed\n");
        return 1;
    }

    if (!check_normal_heap_unchanged(device.get())) return 1;
    if (!check_console_addressed_heap(device.get())) return 1;
    if (!check_xs_proxy_slot14(device.get())) return 1;
    std::fprintf(stderr, "ok\n");
    return 0;
}
