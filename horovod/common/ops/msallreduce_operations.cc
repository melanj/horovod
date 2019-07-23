// Copyright 2016 The TensorFlow Authors. All Rights Reserved.
// Modifications copyright (C) 2019 Microsoft Corp.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
// =============================================================================

#include "msallreduce_operations.h"

namespace horovod {
namespace common {

MsAllreduceOp::MsAllreduceOp(MPIContext* mpi_context, HorovodGlobalState* global_state)
    : PointToPointOp(mpi_context, global_state) {}

Status MsAllreduceOp::Execute(std::vector<TensorTableEntry>& entries, const Response& response) {
  int layerid = 0;
  for (auto& e : entries) {
    void* buffer_data;
    int buffer_len;
    void* recv_buffer;
    buffer_data = (void*) e.tensor->data();
    buffer_len = e.output->size();
    recv_buffer = (void*) e.output->data();
    switch (e.output->dtype()) {
        case HOROVOD_INT8:
        MsAllreduce_Internal((int8_t*) buffer_data,
                            (int8_t*) recv_buffer,
                            buffer_len,
                            Communicator::GLOBAL,
                            layerid,
                            &buffer_len,
                            1);
        break;     
        case HOROVOD_UINT8:
        MsAllreduce_Internal((uint8_t*) buffer_data,
                            (uint8_t*) recv_buffer,
                            buffer_len,
                            Communicator::GLOBAL,
                            layerid,
                            &buffer_len,
                            1);
        break;
        case HOROVOD_UINT16:
        MsAllreduce_Internal((uint16_t*) buffer_data,
                            (uint16_t*) recv_buffer,
                            buffer_len,
                            Communicator::GLOBAL,
                            layerid,
                            &buffer_len,
                            1);
        break;
        case HOROVOD_INT16:
        MsAllreduce_Internal((int16_t*) buffer_data,
                            (int16_t*) recv_buffer,
                            buffer_len,
                            Communicator::GLOBAL,
                            layerid,
                            &buffer_len,
                            1);
        break;
        case HOROVOD_INT32:
        MsAllreduce_Internal((int32_t*) buffer_data,
                            (int32_t*) recv_buffer,
                            buffer_len,
                            Communicator::GLOBAL,
                            layerid,
                            &buffer_len,
                            1);
        break;
        case HOROVOD_INT64:
        MsAllreduce_Internal((int64_t*) buffer_data,
                            (int64_t*) recv_buffer,
                            buffer_len,
                            Communicator::GLOBAL,
                            layerid,
                            &buffer_len,
                            1);
        break;
        case HOROVOD_FLOAT32:
        MsAllreduce_Internal((float*) buffer_data,
                            (float*) recv_buffer,
                            buffer_len,
                            Communicator::GLOBAL,
                            layerid,
                            &buffer_len,
                            1);
        break;
        case HOROVOD_FLOAT64:
        MsAllreduce_Internal((double*) buffer_data,
                            (double*) recv_buffer,
                            buffer_len,
                            Communicator::GLOBAL,
                            layerid,
                            &buffer_len,
                            1);
        break;
        default:
        return Status::InvalidArgument("MS allreduction only supports double, float and float16 types.");
    }

    layerid++;
  }
  LOG(INFO, global_state_->rank)<<"Finished ms allreduction, exiting operation";
  return Status::OK();
}

bool MsAllreduceOp::Enabled(const ParameterManager& param_manager,
                           const std::vector<TensorTableEntry>& entries,
                           const Response& response) const {
  return true;
}

template<typename T>
void MsAllreduceOp::MsAllreduce_Internal(T* gradient_buffer, T* result_buffer, int64_t count, Communicator communicator, int message_tag, int* layer_sizes, int num_layers){
    int true_rank;
    int redn_rank;
    int size;
    MPI_Comm_rank(mpi_context_->GetMPICommunicator(communicator), &true_rank);
    MPI_Comm_size(mpi_context_->GetMPICommunicator(communicator), &size);
    LOG(INFO, global_state_->rank)<<"Starting ms allreduction internal";

    int root_node_rotation = message_tag % size;
    redn_rank = (true_rank + root_node_rotation) % size;

    // Do a tree reduction
    // The reduction ranks used are a permutation of true ranks (permuted based on message_tag) 
    // This spreads the load of tree reduction across different true ranks

    // at each level l, node X0[0..0] receives from X1[0...], 
    // where [0..0] is l zeros in the bit representation of the rank of a node

    int level;
    for (level = 1; level < size; level *= 2) {
        int neighbor_redn_rank = redn_rank ^ level;
        int neighbor_true_rank = (neighbor_redn_rank + size - root_node_rotation) % size;
        if (redn_rank % level != 0)
            continue; // stay idle at this level

        if (neighbor_redn_rank >= size)
            continue; // no neighbor and so stay idle at this level

        if ((redn_rank & level) == 0) {
            // recv buffer from neighbor
            PointToPointRecv(result_buffer, count, neighbor_true_rank, message_tag, communicator);
            PairwiseReduce_Internal<T, T>(gradient_buffer, result_buffer, (int) count, layer_sizes, num_layers);
        }
        else {
            // send gradient_buffer to neighbor
            PointToPointSend(gradient_buffer, count, neighbor_true_rank, message_tag, communicator);
        }
    }

    // Do a inverse tree to do a broadcast
    // cannot use MPI Broadcast as there can be concurrent Allreduces happening in parallel

    // the same logic as above. 
    // at each level l, node X0[0..0] sends to X1[0...], 
    // where [0..0] is l zeros in the bit representation of the rank of a node

    level /= 2; // this make sure that level < size

    for (; level > 0; level /= 2) {
        int neighbor_redn_rank = redn_rank ^ level;
        int neighbor_true_rank = (neighbor_redn_rank + size - root_node_rotation) % size;

        if (redn_rank % level != 0)
            continue; // stay idle at this level

        if (neighbor_redn_rank >= size)
            continue; // no neighbor and so stay idle at this level

        if ((redn_rank & level) == 0) {
            // send gradient_buffer to neighbor
            // and dont wait for the send to finish	      
            PointToPointSend(gradient_buffer, count, neighbor_true_rank, message_tag, communicator);
        }
        else {
            // recv gradient_buffer from neighbor
            PointToPointRecv(gradient_buffer, count, neighbor_true_rank, message_tag, communicator);
        }
    }
}
template<typename T, typename TACC>
void MsAllreduceOp::PairwiseReduce_Internal(T* left_tensor, T* right_tensor, int count, int* layer_sizes, int num_layers){
    LOG(INFO, global_state_->rank)<<"Starting pairwise reduction internal";
    //TODO make this multi-threaded
    //int nt = omp_get_max_threads();
    int nt = 1;

    const int cache_alignment = 64 / sizeof(TACC);

    std::vector<TACC> norms(num_layers*cache_alignment, (TACC)0);
    std::vector<TACC> dotProducts(num_layers*cache_alignment, (TACC)0);

    // reduction is parallelized uniformly across available OpenMP threads
#pragma omp parallel num_threads(nt)
    {
        int tid = omp_get_thread_num();
        int numThreads = omp_get_num_threads();
        int myStart = (count * tid) / numThreads;
        int myEnd = (count * (tid + 1)) / numThreads;

        // go over each layer and process the layer only if it overlaps with [myStart, myEnd)
        for (int i = 0, layer_begin = 0; i < num_layers; layer_begin += layer_sizes[i++]) {
            int layer_end = layer_begin + layer_sizes[i];

            if (myEnd <= layer_begin)
                break; // no overlap now and in future

            if (myStart >= layer_end)
                continue; // no overlap yet

            int begin = std::max(myStart, layer_begin);
            int end = std::min(myEnd, layer_end);

            // compute dotProduct of a[begin, end) and b[begin, end) together with norm square of b[begin, end)
            TACC locDotProduct = 0.f;
            TACC locNorm = 0.f;
            ComputeDotAndNormSqrd(&left_tensor[begin], &right_tensor[begin], end - begin, locDotProduct, locNorm);
#pragma omp critical
            {
                // multiplied by cache_alignment to avoid false sharing
                dotProducts[i * cache_alignment] += locDotProduct;
                norms[i * cache_alignment] += locNorm;
            }
        }
#pragma omp barrier

        // go over each layer and process the layer only if it overlaps with [myStart, myEnd)
        for (int i = 0, layer_begin = 0; i < num_layers; layer_begin += layer_sizes[i++]) {
            int layer_end = layer_begin + layer_sizes[i];

            if (myEnd <= layer_begin)
                break; // no overlap now and in future

            if (myStart >= layer_end)
                continue; // no overlap yet

            TACC dotProduct = dotProducts[i*cache_alignment];
            TACC norm = norms[i*cache_alignment];

            TACC thresh = 1e-18f;
            TACC coeff = 0.0f;
            if (std::abs(norm) < thresh)
                coeff = 1.0f;
            else
                coeff = 1.f - dotProduct / norm;

            int begin = std::max(myStart, layer_begin);
            int end = std::min(myEnd, layer_end);

            // a[begin, end) += coeff * b[begin, end)
            // where coeff = 1 - a.b/normbsq
            TAXPY(end - begin, coeff, &right_tensor[begin], &left_tensor[begin]);
        }
    }
}

template<typename T, typename TACC>
void MsAllreduceOp::ComputeDotAndNormSqrd(const T* __restrict__ a, const T* __restrict__ b, int n, TACC& dotProduct, TACC& normsq){
    dotProduct = 0.;
    normsq = 0.;

    for (int i = 0; i < n; i++) {
        dotProduct += a[i] * b[i];
        normsq += b[i] * b[i];
    }
}

template<typename T, typename TACC>
void MsAllreduceOp::TAXPY(int n, TACC a, T* __restrict__ x, T* __restrict__ y){
    for (int i = 0; i < n; i++) {
        y[i] = a * x[i] + y[i];
    }
}

} // namespace common
} // namespace horovod