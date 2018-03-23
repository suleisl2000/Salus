//
// Created by peifeng on 3/22/18.
//

#include "oplibraries/tensorflow/tensorflow_headers.h"
#include "oplibraries/tensorflow/device/gpu.h"
#include "execution/executionengine.h"
#include "utils/threadutils.h"

#include <utility>

namespace salus::oplib::tensorflow {

class PerTaskGPUDevice : public PerTaskDevice
{
public:
    explicit PerTaskGPUDevice(SalusGPUDevice *base, std::unique_ptr<ResourceContext> &&rctx,
                              NodeStreamMap nsMap);

    tf::DeviceContext *deviceContextForNode(int id) const override;

    ~PerTaskGPUDevice() override;

private:
    NodeStreamMap m_nsMap;
    std::vector<int> m_streams;
};

SalusGPUDevice::SalusGPUDevice(const tf::SessionOptions &options, const std::string &name,
                               tf::Bytes memory_limit, const tf::DeviceLocality &locality, int gpu_id,
                               const std::string &physical_device_desc, tf::Allocator *gpu_allocator,
                               tf::Allocator *cpu_allocator, int max_streams)
    : BaseGPUDevice(options, name, memory_limit, locality, gpu_id, physical_device_desc, gpu_allocator,
                    cpu_allocator, false /* sync every op */, max_streams)
    , m_streamUsed(static_cast<size_t>(max_streams), false)
    , m_streamAssignCache()
{
}

tf::Allocator *SalusGPUDevice::GetAllocator(tf::AllocatorAttributes attr)
{
    if (attr.on_host()) {
        if (attr.gpu_compatible()) {
            return tf::ProcessState::singleton()->GetCUDAHostAllocator(0);
        } else {
            return cpu_allocator_;
        }
    }
    return gpu_allocator_;
}

Status SalusGPUDevice::FillContextMap(const tf::Graph *graph,
                                      std::vector<tf::DeviceContext *> *device_context_map)
{
    UNUSED(device_context_map);

    VLOG(2) << "FillContextMap";

    const auto num_streams = device_contexts_.size();
    // Special case for single stream.
    if (num_streams == 1) {
        return Status::OK();
    }

    NodeStreamMap *node_to_stream_id;
    {
        sstl::Guard g(m_muCache);
        if (m_streamAssignCache.count(graph) > 0) {
            LOG(WARNING) << "Detected graph address reuse: " << as_hex(graph);
        }
        node_to_stream_id = &m_streamAssignCache[graph];
    }

    tf::gpu_stream_util::AssignStreamsOpts opts;
    opts.max_streams = static_cast<int>(num_streams);
    TF_RETURN_IF_ERROR(tf::gpu_stream_util::AssignStreams(graph, opts, node_to_stream_id));

    return Status::OK();
}

void SalusGPUDevice::flushCacheFor(const tf::Graph *graph)
{
    sstl::Guard g(m_muCache);
    m_streamAssignCache.erase(graph);
}

std::unique_ptr<PerTaskDevice> SalusGPUDevice::createPerTaskDevice(const tf::Graph *graph,
                                                                   std::unique_ptr<ResourceContext> &&rctx)
{
    sstl::Guard g(m_muCache);
    return std::make_unique<PerTaskGPUDevice>(this, std::move(rctx), m_streamAssignCache.at(graph));
}

std::vector<int> SalusGPUDevice::allocateStreams(size_t num)
{
    if (num == 0) {
        return {};
    }

    sstl::Guard g(m_muStream);
    std::vector<int> res;
    for (int i = 0; i != max_streams_; ++i) {
        if (!m_streamUsed[i]) {
            res.emplace_back(i);
            m_streamUsed[i] = true;
        }

        if (res.size() == num) {
            break;
        }
    }
    return res;
}

void SalusGPUDevice::freeStreams(const std::vector<int> &streams)
{
    if (streams.empty()) {
        return;
    }

    sstl::Guard g(m_muStream);
    for (auto i : streams) {
        m_streamUsed[i] = false;
    }
}

PerTaskGPUDevice::PerTaskGPUDevice(SalusGPUDevice *base, std::unique_ptr<ResourceContext> &&rctx,
                                   NodeStreamMap nsMap)
    : PerTaskDevice(base, std::move(rctx))
    , m_nsMap(nsMap.size())
{
    // Take and use all gpu streams in staging area
    if (auto scope = resourceContext().alloc(ResourceType::GPU_STREAM)) {
        auto num = scope.resources().at({ResourceType::GPU_STREAM, resourceContext().spec()});
        m_streams = underlayingDevice<SalusGPUDevice>().allocateStreams(num);
        if (m_streams.size() != num) {
            underlayingDevice<SalusGPUDevice>().freeStreams(m_streams);
            scope.rollback();

            LOG(ERROR) << "Can't get enough GPU streams, requested: " << num << " got: " << m_streams.size();
            m_streams.clear();
        }
    }

    // Map logical stream to physical stream using Round-Robin
    if (!m_streams.empty()) {
        std::unordered_map<int, int> lTop;
        lTop.reserve(nsMap.size());
        size_t i = 0;
        for (const auto & [nid, stream] : nsMap) {
            auto phy = sstl::optionalGet(lTop, stream);
            if (!phy) {
                phy = m_streams[i++];
                if (i >= m_streams.size()) {
                    i = 0;
                }
            }
            m_nsMap[nid] = *phy;
        }
    }
}

tf::DeviceContext *PerTaskGPUDevice::deviceContextForNode(int id) const
{
    auto &device = underlayingDevice<SalusGPUDevice>();
    auto it = m_nsMap.find(id);
    if (it == m_nsMap.end()) {
        return device.device_contexts_[0];
    }

    DCHECK_LT(it->second, static_cast<int>(device.device_contexts_.size()));
    return device.device_contexts_[it->second];
}

PerTaskGPUDevice::~PerTaskGPUDevice()
{
    underlayingDevice<SalusGPUDevice>().freeStreams(m_streams);
}

tf::BaseGPUDevice *SalusGPUDeviceFactory::CreateGPUDevice(const tf::SessionOptions &options,
                                                          const std::string &name, tf::Bytes memory_limit,
                                                          const tf::DeviceLocality &locality, int gpu_id,
                                                          const std::string &physical_device_desc,
                                                          tf::Allocator *gpu_allocator,
                                                          tf::Allocator *cpu_allocator)
{
    return new SalusGPUDevice(options, name, memory_limit, locality, gpu_id, physical_device_desc,
                              gpu_allocator, cpu_allocator);
}

} // namespace salus::oplib::tensorflow
