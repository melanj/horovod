//TODO license
#include "adasum_cuda_operations.h"
#include <boost/asio/post.hpp>

namespace horovod {
namespace common {

std::unordered_map<std::thread::id, std::array<double*, 3>> AdasumCudaAllreduceOp::thread_to_device_variable_map;

AdasumCudaAllreduceOp::AdasumCudaAllreduceOp(MPIContext* mpi_context, NCCLContext* nccl_context, CUDAContext* cuda_context, HorovodGlobalState* global_state)
    : AdasumMPIOp(mpi_context, global_state), nccl_context_(nccl_context), cuda_context_(cuda_context) {
}

AdasumCudaAllreduceOp::~AdasumCudaAllreduceOp() {
  FreeDeviceVariables();
}

void AdasumCudaAllreduceOp::InitCUDAStreams(const std::vector<TensorTableEntry> entries) {

  auto first_entry = entries[0];
  cuda_context_->ErrorCheck("cudaSetDevice", cudaSetDevice(first_entry.device));

  // Ensure streams are in the map before executing reduction.
  for(int i = 0; i < entries.size(); i++) {
    cudaStream_t& stream = cuda_context_->streams[global_state_->current_nccl_stream][i];
    if (stream == nullptr) {

      int greatest_priority;
      cuda_context_->ErrorCheck("cudaDeviceGetStreamPriorityRange",
                                cudaDeviceGetStreamPriorityRange(NULL, &greatest_priority));
      cuda_context_->ErrorCheck("cudaStreamCreateWithPriority",
                                cudaStreamCreateWithPriority(&stream, cudaStreamNonBlocking, greatest_priority));
    }
  }
  cudaStream_t& device_stream = cuda_context_->streams[global_state_->current_nccl_stream][first_entry.device];
  if (device_stream == nullptr) {
    int greatest_priority;
    cuda_context_->ErrorCheck("cudaDeviceGetStreamPriorityRange",
                              cudaDeviceGetStreamPriorityRange(NULL, &greatest_priority));
    cuda_context_->ErrorCheck("cudaStreamCreateWithPriority",
                              cudaStreamCreateWithPriority(&device_stream, cudaStreamNonBlocking, greatest_priority));
  }
}

void AdasumCudaAllreduceOp::InitDeviceVariables() {
  auto thread_id = std::this_thread::get_id();
  if (thread_to_device_variable_map.find(thread_id) == thread_to_device_variable_map.end() &&
      thread_to_device_variable_map[thread_id][0] == nullptr)
  {
    double* device_normsq_memory_a;
    double* device_normsq_memory_b;
    double* device_dot_product_memory;
    cuda_context_->ErrorCheck("cudaMalloc",
                            cudaMalloc(&device_normsq_memory_a, sizeof(double)));
    cuda_context_->ErrorCheck("cudaMalloc",
                            cudaMalloc(&device_normsq_memory_b, sizeof(double)));
    cuda_context_->ErrorCheck("cudaMalloc",
                            cudaMalloc(&device_dot_product_memory, sizeof(double)));
    std::lock_guard<std::mutex> guard(buffer_lock_);
    thread_to_device_variable_map[thread_id][0] = device_normsq_memory_a;
    thread_to_device_variable_map[thread_id][1] = device_normsq_memory_b;
    thread_to_device_variable_map[thread_id][2] = device_dot_product_memory;
  }
}

void AdasumCudaAllreduceOp::FreeDeviceVariables() {
  if (!thread_to_device_variable_map.empty()){
    for (auto it = thread_to_device_variable_map.begin(); it != thread_to_device_variable_map.end(); ++it)
    {
      if(it->second[0] != nullptr) {
        cudaFree(it->second[0]);
        cudaFree(it->second[1]);
        cudaFree(it->second[2]);
      }
    }
  }
}

void AdasumCudaAllreduceOp::InitNCCLComm(const std::vector<TensorTableEntry>& entries,
                                         const std::vector<int32_t>& nccl_device_map) {
  // Ensure NCCL communicator is in the map before executing reduction.
  ncclComm_t& nccl_comm = nccl_context_->nccl_comms[global_state_->current_nccl_stream][nccl_device_map];
  if (nccl_comm == nullptr) {
    auto& timeline = global_state_->timeline;
    timeline.ActivityStartAll(entries, INIT_NCCL);

    int nccl_rank = global_state_->controller->GetLocalRank();
    int nccl_size = global_state_->controller->GetLocalSize();
    Communicator nccl_id_bcast_comm = Communicator::LOCAL;

    ncclUniqueId nccl_id;
    if (nccl_rank == 0) {
      nccl_context_->ErrorCheck("ncclGetUniqueId", ncclGetUniqueId(&nccl_id));
    }

    global_state_->controller->Bcast((void*)&nccl_id, sizeof(nccl_id), 0,
                                     nccl_id_bcast_comm);

    ncclComm_t new_nccl_comm;
    auto nccl_result = ncclCommInitRank(&new_nccl_comm, nccl_size, nccl_id, nccl_rank);
    nccl_context_->ErrorCheck("ncclCommInitRank", nccl_result);
    nccl_comm = new_nccl_comm;

    // Barrier helps NCCL to synchronize after initialization and avoid
    // deadlock that we've been seeing without it.
    global_state_->controller->Barrier(Communicator::GLOBAL);

    timeline.ActivityEndAll(entries);
  }

  nccl_comm_ = &nccl_comm;
}

Status AdasumCudaAllreduceOp::Execute(std::vector<TensorTableEntry>& entries, const Response& response) {
  if(entries.size() < 1) {
    return Status::OK();
  }
  InitCUDAStreams(entries);
  if(global_state_->adasum_algorithm == AdasumAlgorithm::GPU_TREE) {
    return TreeHierarchical(entries, response);
  }
  else if(global_state_->adasum_algorithm == AdasumAlgorithm::GPU_RING) {
    return RingHierarchical(entries, response);
  }
  else if(global_state_->adasum_algorithm == AdasumAlgorithm::GPU_NCCL_SUM_RING) {
    return NcclHierarchical(entries, response);
  }
  else {
    throw std::logic_error("Unsupported adasum reduction algorithm");
  }  
}

Status AdasumCudaAllreduceOp::NcclHierarchical(std::vector<TensorTableEntry>& entries, const Response& response) {

  MPI_Comm* node_comm = NULL;
  if (rank_log_size_ != 0) {
    node_comm = &reduction_comms_[rank_log_size_-1];
  }

  // Determine GPU IDs of the devices participating in this communicator.
  std::vector<int32_t> nccl_device_map;
  nccl_device_map.reserve(
      global_state_->controller->GetLocalCommRanks().size());
  for (int rank : global_state_->controller->GetLocalCommRanks()) {
    nccl_device_map.push_back(response.devices()[rank]);
  }

  InitNCCLComm(entries, nccl_device_map);

  bool do_cross_comm = global_state_->controller->GetLocalRank() == 0 && node_comm != NULL;

  std::vector<std::unique_ptr<char[]>> host_buffers;
  std::vector<cudaEvent_t> events(entries.size());

  for (int i = 0; i < entries.size(); ++i) {
    auto& entry = entries.at(i);
    const void* input_data = entry.tensor->data();
    void* buffer_data = (void*) entry.output->data();
    size_t buffer_len = (size_t) entry.output->size();
    int num_elements = entry.tensor->shape().num_elements();

    auto nccl_result = ncclReduce(input_data,
                                  buffer_data,
                                  (size_t) num_elements,
                                  GetNCCLDataType(entry.tensor),
                                  ncclSum, 0, *nccl_comm_, 
                                  cuda_context_->streams[global_state_->current_nccl_stream][entry.device]);
    nccl_context_->ErrorCheck("ncclReduce", nccl_result);

    if (do_cross_comm) {
      host_buffers.emplace_back(new char[buffer_len]);
      void* host_buffer = (void*)host_buffers.at(i).get();

      cuda_context_->ErrorCheck("cudaMemcpyAsync",
                                cudaMemcpyAsync(host_buffer, buffer_data,
                                                buffer_len, cudaMemcpyDeviceToHost,
                                                cuda_context_->streams[global_state_->current_nccl_stream][entry.device]));

      auto& event = events.at(i);
      cuda_context_->ErrorCheck("GetCudaEvent", cuda_context_->GetCudaEvent(&event));
      cuda_context_->ErrorCheck("cudaEventRecord", cudaEventRecord(event, 
                                cuda_context_->streams[global_state_->current_nccl_stream][entry.device]));
    }
  }

  if (do_cross_comm) {
    std::vector<char> recv_buffer;
    for (int i = 0; i < entries.size(); ++i) {
      auto& entry = entries.at(i);
      void* buffer_data = (void*) entry.output->data();
      size_t buffer_len = (size_t) entry.output->size();
      int num_elements = entry.tensor->shape().num_elements();
      auto host_buffer = (void*)host_buffers.at(i).get();
      auto& event = events.at(i);

      cuda_context_->ErrorCheck("cudaEventSynchronize", cudaEventSynchronize(event));
      cuda_context_->ErrorCheck("ReleaseCudaEvent", cuda_context_->ReleaseCudaEvent(event));

      recv_buffer.resize(buffer_len);
      DispatchSyncAllreduce(host_buffer, recv_buffer.data(), node_comm, reduction_comms_, i, entry);

      cuda_context_->ErrorCheck("cudaMemcpyAsync",
                                cudaMemcpyAsync(buffer_data, host_buffer,
                                                buffer_len, cudaMemcpyHostToDevice,
                                                cuda_context_->streams[global_state_->current_nccl_stream][entry.device]));
    }
  }

  for (int i = 0; i < entries.size(); ++i) {
    auto& entry = entries.at(i);
    void* buffer_data = (void*) entry.output->data();
    int num_elements = entry.tensor->shape().num_elements();

    nccl_context_->ErrorCheck("ncclBcast",
                              ncclBcast(buffer_data,
                                        (size_t) num_elements,
                                        GetNCCLDataType(entry.tensor),
                                        0,
                                        *nccl_comm_, 
                                        cuda_context_->streams[global_state_->current_nccl_stream][entry.device]));
    auto& event = events.at(i);
    cuda_context_->ErrorCheck("GetCudaEvent", cuda_context_->GetCudaEvent(&event));
    cuda_context_->ErrorCheck("cudaEventRecord", cudaEventRecord(event,
                              cuda_context_->streams[global_state_->current_nccl_stream][entry.device]));
  }

  for (int i = 0; i < entries.size(); ++i) {
    auto& event = events.at(i);
    cuda_context_->ErrorCheck("cudaEventSynchronize", cudaEventSynchronize(event));
    cuda_context_->ErrorCheck("ReleaseCudaEvent", cuda_context_->ReleaseCudaEvent(event));
  }

  return Status::OK();
}

Status AdasumCudaAllreduceOp::RingHierarchical(std::vector<TensorTableEntry>& entries,
                        const Response& response) {

  int num_reductions = entries.size();
	AllRings all_rings(global_state_->controller->GetLocalRank(), global_state_->controller->GetLocalSize());
  std::deque<FusionBufferManager> used_buffer_managers;
  for (size_t layerid = 0; layerid < entries.size(); ++layerid) {
    auto& entry = entries.at(layerid);
    void* buffer_data;
    int buffer_len;
    void* recv_buffer;

    buffer_data = (void*) entry.tensor->data();

    buffer_len = entry.output->size();

    if(entry.tensor->data() == entry.output->data()) {

        // Get the temp buffer to be used for the Op
        FusionBufferManager buffer_manager;
        if (!temp_buffers_.empty()) {
          buffer_manager = temp_buffers_.front();
          temp_buffers_.pop_front();
        }
        used_buffer_managers.push_back(buffer_manager);

        // TODO: Maybe add before and after callbacks to timeline?
        Status status = buffer_manager.InitializeBuffer(
            buffer_len,
            entry.device, entry.context,
            global_state_->current_nccl_stream,
            []() {},
            []() {},
            [](int64_t& size, int64_t& threshold) {return size >= threshold;});

        if (!status.ok()) {
            throw std::logic_error("AdasumOp::Execute_helper: Initialize buffer failed.");
        }
        auto buffer = buffer_manager.GetBuffer(entry.device, entry.context->framework(), global_state_->current_nccl_stream);
        recv_buffer = const_cast<void*>(buffer->AccessData(entry.context));
    }
    else {
        recv_buffer = (void*) entry.output->data();
    }
  
    all_rings.InitMessageInRing(new ReduceMessage(mpi_context_),
                      buffer_data,
                      recv_buffer,
                      buffer_len,
                      entry.tensor->dtype(),
                      mpi_context_->local_comm,
                      layerid,
                      global_state_->controller->GetLocalRank());
  }
  all_rings.WaitAllMessages();
  // Return used buffer managers to the queue
  temp_buffers_.insert(temp_buffers_.end(), used_buffer_managers.begin(), used_buffer_managers.end());

  int local_rank = 0;
  MPI_Comm_rank(mpi_context_->local_comm, &local_rank);
  if (local_rank == 0 && rank_log_size_ != 0) {
    std::vector<std::unique_ptr<char[]>> allreduce_buffers;

    // start device to host copies
    for (size_t layerid = 0; layerid < entries.size(); ++layerid) {
      auto& entry = entries.at(layerid);
      int buffer_len = entry.output->size();
      allreduce_buffers.emplace_back(new char[buffer_len]);
      char* buffer_data = allreduce_buffers.at(layerid).get();
      
      auto cuda_result = cudaMemcpyAsync(
        buffer_data, (void*) entry.tensor->data(),
        buffer_len, 
        cudaMemcpyDeviceToHost,
        cuda_context_->streams[global_state_->current_nccl_stream][layerid]);
      cuda_context_->ErrorCheck("cudaMemcpyAsync", cuda_result);
    }

    for (size_t layerid = 0; layerid < entries.size(); ++layerid) {
      auto& entry = entries.at(layerid);
      int buffer_len = entry.output->size();
      char* buffer_data = allreduce_buffers.at(layerid).get();
      std::unique_ptr<char[]> recv_buffer(new char[buffer_len]);

      // wait for this layer to finish copying to host
      auto cuda_result = cudaStreamSynchronize(cuda_context_->streams[global_state_->current_nccl_stream][layerid]);
      cuda_context_->ErrorCheck("cudaStreamSynchronize", cuda_result);

      MPI_Comm* node_comm = &reduction_comms_[rank_log_size_-1];
      DispatchSyncAllreduce(buffer_data, recv_buffer.get(), node_comm, reduction_comms_, layerid, entry);
      // start the copy back to device
      cuda_result = cudaMemcpyAsync(
        (void*) entry.tensor->data(), buffer_data,
        buffer_len, 
        cudaMemcpyHostToDevice,
        cuda_context_->streams[global_state_->current_nccl_stream][layerid]);
      cuda_context_->ErrorCheck("cudaMemcpyAsync", cuda_result);
    }

    // wait for all copies to device to finish
    for (size_t layerid = 0; layerid < entries.size(); ++layerid) {
      auto cuda_result = cudaStreamSynchronize(cuda_context_->streams[global_state_->current_nccl_stream][layerid]);
      cuda_context_->ErrorCheck("cudaStreamSynchronize", cuda_result);
    }
  }

  for (size_t layerid = 0; layerid < entries.size(); ++layerid) {
    auto& entry = entries.at(layerid);
    void* buffer_data;
    int buffer_len;

    buffer_data = (void*) entry.tensor->data();

    buffer_len = entry.output->size();

    all_rings.InitMessageInRing(new BroadcastMessage(mpi_context_),
                      buffer_data,
                      nullptr,
                      buffer_len,
                      entry.output->dtype(),
                      mpi_context_->local_comm,
                      layerid,
                      global_state_->controller->GetLocalRank());
  }
  all_rings.WaitAllMessages();
  for (size_t layerid = 0; layerid < entries.size(); ++layerid) {
    auto& entry = entries.at(layerid);
    if(entry.tensor->data() != entry.output->data()) {
      MemcpyUtil(entry, (void *) entry.output->data(), (void *) entry.tensor->data(), (size_t) entry.tensor->size(), layerid);
    }
  }

  return Status::OK();
}

void AdasumCudaAllreduceOp::MemcpyUtil(TensorTableEntry entry, void* dest, void* src, size_t buffer_len, int layerid) {
    assert(dest != nullptr);
    assert(src != nullptr);
    auto cuda_result = cudaMemcpyAsync(dest, src,
                                    buffer_len, 
                                    cudaMemcpyDeviceToDevice,
                                    cuda_context_->streams[global_state_->current_nccl_stream][entry.device]);
    cuda_context_->ErrorCheck("cudaMemcpyAsync", cuda_result);
    auto cuda_sync_result = cudaStreamSynchronize(cuda_context_->streams[global_state_->current_nccl_stream][entry.device]);
    cuda_context_->ErrorCheck("cudaStreamSynchronize", cuda_sync_result);
}

void AdasumCudaAllreduceOp::DispatchComputeDotAndNormSqrds(const void* __restrict__ a,
                                                          const void* __restrict__ b,
                                                          DataType horovod_datatype,
                                                          int count,
                                                          double& dotProduct,
                                                          double& anormsq,
                                                          double& bnormsq,
                                                          HorovodGlobalState *global_state,
                                                          int layerid) {
  if (horovod_datatype == DataType::HOROVOD_FLOAT16) {
    DotProductImpl((uint16_t*)a, (uint16_t*)b, count, dotProduct, anormsq, bnormsq, global_state_, layerid);
  }
  else if (horovod_datatype == DataType::HOROVOD_FLOAT32) {
    DotProductImpl((float*)a, (float*)b, count, dotProduct, anormsq, bnormsq, global_state_, layerid);
  }
  else if (horovod_datatype == DataType::HOROVOD_FLOAT64) {
    DotProductImpl((double*)a, (double*)b, count, dotProduct, anormsq, bnormsq, global_state_, layerid);      
  }
  else {
      throw std::logic_error("Unsupported data type.");
  }

}

void AdasumCudaAllreduceOp::DispatchScaledAdd(DataType horovod_datatype,
                                             int count,
                                             double acoeff,
                                             void* __restrict__ a,
                                             double bcoeff,
                                             void* __restrict__ b,
                                             HorovodGlobalState *global_state,
                                             int layerid) {
  if (horovod_datatype == DataType::HOROVOD_FLOAT16) {
    ScaleAddImpl(count, acoeff, (uint16_t*)a, bcoeff, (uint16_t*)b, global_state_, layerid);
  }
  else if (horovod_datatype == DataType::HOROVOD_FLOAT32) {
    ScaleAddImpl(count, acoeff, (float*)a, bcoeff, (float*)b, global_state_, layerid);
  }
  else if (horovod_datatype == DataType::HOROVOD_FLOAT64) {
    ScaleAddImpl(count, acoeff, (double*)a, bcoeff, (double*)b, global_state_, layerid);
  }
  else {
      throw std::logic_error("Unsupported data type.");
  }
  
}

bool AdasumCudaAllreduceOp::Enabled(const ParameterManager& param_manager,
                            const std::vector<TensorTableEntry>& entries,
                            const Response& response) const {
  return entries[0].device != CPU_DEVICE_ID && global_state_->adasum_algorithm != AdasumAlgorithm::NONE;

}
}
}