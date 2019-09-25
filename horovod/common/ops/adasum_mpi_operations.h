//TODO license
#ifndef HOROVOD_ADASUM_MPI_OPERATIONS_H
#define HOROVOD_ADASUM_MPI_OPERATIONS_H

#include <iostream>
#include "mpi.h"

#include "../mpi/mpi_context.h"
#include "adasum_operations.h"


namespace horovod {
namespace common {

class AdasumMPIOp : public AdasumOp<MPI_Comm> {
public:
  AdasumMPIOp(MPIContext* mpi_context, HorovodGlobalState* global_state);
  
  ~AdasumMPIOp();
  
  Status Execute(std::vector<TensorTableEntry>& entries,
                         const Response& response) override;

  bool Enabled(const ParameterManager& param_manager,
                       const std::vector<TensorTableEntry>& entries,
                       const Response& response) const override;
protected:
  void PointToPointSend(void* input_data_buffer,
                        int64_t buffer_length,
                        DataType horovod_datatype,
                        int dest_rank,
                        int tag,
                        MPI_Comm communicator) override;

  void PointToPointRecv(void* output_data_buffer,
                        int64_t buffer_length,
                        DataType horovod_datatype,
                        int src_rank,
                        int tag,
                        MPI_Comm communicator) override;

  void PointToPointSendRecv(void* input_data_buffer,
                            int64_t input_buffer_length,
                            DataType input_horovod_datatype,
                            int dst_rank,
                            int send_tag,
                            void* output_data_buffer,
                            int64_t output_buffer_length,
                            DataType output_horovod_datatype,
                            int src_rank,
                            int recv_tag,
                            MPI_Comm communicator) override;

  void P2pAllreduce(void *grad_buffer, 
                    void *recv_buffer, 
                    int64_t buffer_length, 
                    DataType horovod_datatype,
                    MPI_Comm communicator, 
                    int message_tag) override;

  template<typename T> 
  inline void ElementwiseAdd(T *grad_buffer, T *recv_buffer, int count) {
    for(int i = 0; i < count; i++) {
      grad_buffer[i] += recv_buffer[i];
    }
  }

  int GetLocalRankWithComm(MPI_Comm local_comm) override;

  int GetSizeWithComm(MPI_Comm comm) override;

  virtual void InitDeviceVariables();

  Status TreeHierarchical(std::vector<TensorTableEntry>& entries,
                          const Response& response);

  void DispatchComputeDotAndNormSqrds(const void* __restrict__  a,
                                      const void* __restrict__ b,
                                      DataType horovod_datatype,
                                      int count,
                                      double& dotProduct,
                                      double& anormsq,
                                      double& bnormsq,
                                      HorovodGlobalState *global_state,
                                      int layerid) override;

  void DispatchScaledAdd(DataType horovod_datatype,
                         int count,
                         double acoeff,
                         void* __restrict__ a,
                         double bcoeff,
                         void* __restrict__ b,
                         HorovodGlobalState *global_state,
                         int layerid) override;
protected:
  MPIContext* mpi_context_;
  int rank_log_size_;
  // MPI communicators used to do adasum
  MPI_Comm* reduction_comms_ = nullptr;
private:
  void TreeHierarchicalInternal(TensorTableEntry& entry, int layerid, const Response& response);
};

} // namespace common
} // namespace horovod

#endif // HOROVOD_ADASUM_MPI_OPERATIONS_H