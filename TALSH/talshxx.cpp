/** ExaTensor::TAL-SH: Device-unified user-level C++ API implementation.
REVISION: 2019/02/07

Copyright (C) 2014-2019 Dmitry I. Lyakh (Liakh)
Copyright (C) 2014-2019 Oak Ridge National Laboratory (UT-Battelle)

This file is part of ExaTensor.

ExaTensor is free software: you can redistribute it and/or modify
it under the terms of the GNU Lesser General Public License as published
by the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

ExaTensor is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
GNU Lesser General Public License for more details.

You should have received a copy of the GNU Lesser General Public License
along with ExaTensor. If not, see <http://www.gnu.org/licenses/>.
------------------------------------------------------------------------
**/

#include "talshxx.hpp"

namespace talsh{

//Static constant storage:

constexpr float TensorData<float>::unity;
constexpr float TensorData<float>::zero;
constexpr double TensorData<double>::unity;
constexpr double TensorData<double>::zero;
constexpr std::complex<float> TensorData<std::complex<float>>::unity;
constexpr std::complex<float> TensorData<std::complex<float>>::zero;
constexpr std::complex<double> TensorData<std::complex<double>>::unity;
constexpr std::complex<double> TensorData<std::complex<double>>::zero;


//Helper functions:
// Generic real/imaginary part extraction:
double realPart(float number){return static_cast<double>(number);}
double realPart(double number){return number;}
double realPart(std::complex<float> number){return static_cast<double>(number.real());}
double realPart(std::complex<double> number){return number.real();}
double imagPart(float number){return 0.0f;}
double imagPart(double number){return 0.0;}
double imagPart(std::complex<float> number){return static_cast<double>(number.imag());}
double imagPart(std::complex<double> number){return number.imag();}


//Functions:

Tensor::Impl::~Impl()
{
 if(used_ != 0) std::cout << "#ERROR(Tensor::Impl::~Impl): Non-zero use count = " << used_ << std::endl;
 if(write_task_ != nullptr) std::cout << "#ERROR(Tensor::Impl::~Impl): Non-null task pointer = " << (void*)write_task_ << std::endl;
 assert(used_ == 0 && write_task_ == nullptr);
 int errc = talshTensorDestruct(&tensor_);
 assert(errc == TALSH_SUCCESS);
}


/** Returns the tensor rank (order in math terms). **/
int Tensor::getRank() const
{
 return talshTensorRank(&(pimpl_->tensor_));
}

/** Returns the tensor order (rank in phys terms). **/
int Tensor::getOrder() const
{
 return this->getRank();
}


/** Use counter increment. **/
Tensor & Tensor::operator++()
{
 ++(pimpl_->used_);
 return *this;
}

/** Use counter decrement. **/
Tensor & Tensor::operator--()
{
 assert(pimpl_->used_ > 0);
 --(pimpl_->used_);
 return *this;
}


/** Synchronizes the tensor presence on a given device.
    Returns TRUE on success, FALSE if an active write task
    on this tensor has failed to complete successfully. **/
bool Tensor::sync(const int device_kind, const int device_id, void * dev_mem)
{
 bool res = this->complete_write_task();
 if(res){
  int errc;
  if(dev_mem != nullptr){ //client provided an explicit buffer to place the tensor into
   errc = talshTensorPlace(&(pimpl_->tensor_),device_id,device_kind,dev_mem);
  }else{ //no explicit buffer provided, use saved information (if any)
   if(device_kind == DEV_HOST){
    errc = talshTensorPlace(&(pimpl_->tensor_),device_id,device_kind,pimpl_->host_mem_);
   }else{
    errc = talshTensorPlace(&(pimpl_->tensor_),device_id,device_kind);
   }
  }
  assert(errc == TALSH_SUCCESS);
 }
 return res;
}


/** Prints the tensor. **/
void Tensor::print() const
{
 std::cout << "TAL-SH Tensor {";
 std::size_t rank = (pimpl_->signature_).size();
 for(std::size_t i = 0; i < rank - 1; ++i) std::cout << (pimpl_->signature_).at(i) << ",";
 if(rank > 0) std::cout << (pimpl_->signature_).at(rank-1);
 std::cout << "} [use=" << pimpl_->used_ << "]:" << std::endl;
 talshTensorPrintInfo(&(pimpl_->tensor_));
 return;
}


talsh_tens_t * Tensor::get_talsh_tensor_ptr()
{
 return &(pimpl_->tensor_);
}


/** Completes the current write task on the tensor, if any. **/
bool Tensor::complete_write_task()
{
 bool res = true;
 if(pimpl_->write_task_ != nullptr){
  res = pimpl_->write_task_->wait();
  pimpl_->write_task_ = nullptr;
 }
 return res;
}


/** Initializes TAL-SH runtime. **/
void initialize(std::size_t * host_buffer_size)
{
 int num_gpu, gpu_list[MAX_GPUS_PER_NODE];
 int errc = talshDeviceCount(DEV_NVIDIA_GPU,&num_gpu);
 assert(errc == TALSH_SUCCESS && num_gpu >= 0);
 if(num_gpu > 0){for(int i = 0; i < num_gpu; ++i) gpu_list[i]=i;};

 int host_arg_max;
 if(host_buffer_size == nullptr){
  std::size_t buf_size = DEFAULT_HOST_BUFFER_SIZE;
  errc = talshInit(&buf_size,&host_arg_max,num_gpu,gpu_list,0,NULL,0,NULL);
 }else{
  errc = talshInit(host_buffer_size,&host_arg_max,num_gpu,gpu_list,0,NULL,0,NULL);
 }
 if(errc != TALSH_SUCCESS) std::cout << "#ERROR(talshInit): TAL-SH initialization error " << errc << std::endl;
 assert(errc == TALSH_SUCCESS);
 return;
}


/** Shutsdown TAL-SH runtime. **/
void shutdown()
{
 int errc = talshShutdown();
 assert(errc == TALSH_SUCCESS);
 return;
}


} //namespace talsh
