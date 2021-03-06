#include "jtorch/spatial_divisive_normalization.h"
#include "jtorch/tensor.h"
#include "jcl/threading/thread.h"
#include "jcl/threading/callback.h"
#include "jcl/threading/thread_pool.h"
#include "jcl/data_str/vector_managed.h"

#define SAFE_DELETE(x) if (x != NULL) { delete x; x = NULL; }
#define SAFE_DELETE_ARR(x) if (x != NULL) { delete[] x; x = NULL; }

using namespace jcl::threading;
using namespace jcl::math;
using namespace jcl::data_str;

namespace jtorch {

  // kernel1d default is either TorchStage::gaussian1D<float>(n) or just a
  // vector of 1 values.
  SpatialDivisiveNormalization::SpatialDivisiveNormalization(
    const Tensor<float>& kernel, const float threshold) : TorchStage() {
    if (kernel.dim() > 2) {
      throw std::runtime_error("SpatialSubtractiveNormalization() - ERROR: "
        "Averaging kernel must be 1D or 2D!");
    }
    if (kernel.size()[0] % 2 == 0 || 
      (kernel.dim() == 2 && kernel.size()[1] % 2 == 0)) {
        throw std::runtime_error("SpatialSubtractiveNormalization() - ERROR: "
          "Averaging kernel must have odd size!");
    }

    kernel_ = Tensor<float>::clone(kernel);
    kernel_norm_ = NULL;   // Normalization is input size dependant

    output = NULL;
    std_coef_ = NULL;
    std_pass1_ = NULL;
    std_pass2_ = NULL;
    std_ = NULL;

    threshold_ = threshold;
  }

  SpatialDivisiveNormalization::~SpatialDivisiveNormalization() {
    cleanup();
	SAFE_DELETE(kernel_);
	SAFE_DELETE(kernel_norm_);
  }

  void SpatialDivisiveNormalization::cleanup() {
    SAFE_DELETE(output);    
    SAFE_DELETE(std_coef_);
    SAFE_DELETE(std_pass1_);
    SAFE_DELETE(std_pass2_);
    SAFE_DELETE(std_);
  }

  void SpatialDivisiveNormalization::init(TorchData& input)  {
    if (input.type() != TorchDataType::TENSOR_DATA) {
      throw std::runtime_error("SpatialDivisiveNormalization::init() - "
        "FloatTensor expected!");
    }
    Tensor<float>& in = (Tensor<float>&)input;

    if (in.dim() != 3) {
      throw std::runtime_error("SpatialDivisiveNormalization::init() - "
        "3D input is expected!");
    }

    if (output != NULL) {
      if (!in.isSameSizeAs(*(Tensor<float>*)output)) {
        // Input dimension has changed!
        cleanup();
      }
    }

    if (output == NULL) {
      output = new Tensor<float>(in.dim(), in.size());
      std_pass1_ = new Tensor<float>(in.dim(), in.size());
      std_pass2_ = new Tensor<float>(in.dim(), in.size());

      //cl_context->getOptimalLocalWorkgroupSizes(deviceid, 
      //  TO_TENSOR_PTR(output)->dim(), local_worgroup_size_3d);
    }
    if (kernel_norm_ == NULL) {
      bool onedim_kernel = kernel_->dim() == 1;
      const float n_feats = (float)in.size()[2];

      // Clone and normalize the input kernel
      kernel_norm_ = Tensor<float>::clone(*kernel_);
      float sum = Tensor<float>::slowSum(*kernel_norm_);
      float div_val = onedim_kernel ? (sum * sqrtf(n_feats)) : (sum * n_feats);
      Tensor<float>::div(*kernel_norm_, div_val);
    }
    if (std_coef_ == NULL) {
      uint32_t std_coeff_size[2];
      std_coeff_size[0] = TO_TENSOR_PTR(output)->size()[0];
      std_coeff_size[1] = TO_TENSOR_PTR(output)->size()[1];
      std_coef_ = new Tensor<float>(2, std_coeff_size);

      float* std_coef_cpu = new float[std_coef_->nelems()];
      float* kernel_norm_cpu = new float[kernel_norm_->nelems()];
      kernel_norm_->getData(kernel_norm_cpu);
      bool onedim_kernel = kernel_->dim() == 1;

      // Filter an image of all 1 values to create the normalization constants
      // See norm_test.lua for proof that this works as well as:
      // https://github.com/andresy/torch/blob/master/extra/nn/SpatialDivisiveNormalization.lua
      int32_t n_feats = TO_TENSOR_PTR(output)->size()[2];
      int32_t height = TO_TENSOR_PTR(output)->size()[1];
      int32_t width = TO_TENSOR_PTR(output)->size()[0];
      if (onedim_kernel) {
        // 1D case - The filter is seperable, but we'll just do the dumb 2D 
        // version since we only do this once on startup.  --> O(n * m)
        int32_t kernel_size = kernel_norm_->size()[0];
        int32_t filt_rad = (kernel_size - 1) / 2;
        for (int32_t v = 0; v < height; v++) {
          for (int32_t u = 0; u < width; u++) {
            float tmp = 0.0f;
            for (int32_t v_filt = -filt_rad; v_filt <= filt_rad; v_filt++) {
              for (int32_t u_filt = -filt_rad; u_filt <= filt_rad; u_filt++) {
                int32_t u_in = u + u_filt;
                int32_t v_in = v + v_filt;
                if (u_in >= 0 && u_in < width && v_in >= 0 && v_in < height) {
                  // Pixel is inside --> We'll effectively clamp zeros elsewhere.
                  tmp += 
                    (kernel_norm_cpu[v_filt + filt_rad] * 
                     kernel_norm_cpu[u_filt + filt_rad]);
                }
              }
            }
            std_coef_cpu[v * width + u] = tmp / n_feats;
          }
        }
      } else {
        // 2D case
        int32_t kernel_size_u = kernel_norm_->size()[0];
        int32_t kernel_size_v = kernel_norm_->size()[1];
        int32_t filt_rad_u = (kernel_size_u - 1) / 2;
        int32_t filt_rad_v = (kernel_size_v - 1) / 2;
        for (int32_t v = 0; v < height; v++) {
          for (int32_t u = 0; u < width; u++) {
            float tmp = 0.0f;
            for (int32_t v_filt = -filt_rad_v; v_filt <= filt_rad_v; v_filt++) {
              for (int32_t u_filt = -filt_rad_u; u_filt <= filt_rad_u; u_filt++) {
                int32_t u_in = u + u_filt;
                int32_t v_in = v + v_filt;
                if (u_in >= 0 && u_in < width && v_in >= 0 && v_in < height) {
                  // Pixel is inside --> We'll effectively clamp zeros elsewhere.
                  tmp += 
                    kernel_norm_cpu[(v_filt + filt_rad_v) * kernel_size_u + (u_filt + filt_rad_u)];
                }
              }
            }
            std_coef_cpu[v * width + u] = tmp / n_feats;
          }
        }
      }
      std_coef_->setData(std_coef_cpu);
      delete[] std_coef_cpu;
      delete[] kernel_norm_cpu;
    }
    if (std_ == NULL) {
      uint32_t std_coeff_size[2];
      std_coeff_size[0] = TO_TENSOR_PTR(output)->size()[0];
      std_coeff_size[1] = TO_TENSOR_PTR(output)->size()[1];
      std_ = new Tensor<float>(2, std_coeff_size);

      //cl_context->getOptimalLocalWorkgroupSizes(deviceid, std_->dim(), 
      //  local_worgroup_size_2d);
    }
  }

  void SpatialDivisiveNormalization::forwardProp(TorchData& input) { 
    init(input);
    bool onedim_kernel = kernel_->dim() == 1;

    std::string kernel = jtorch::jtorch_path + 
      "kernels/spatial_divisive_normalization.cl";
    Tensor<float>& in = (Tensor<float>&)input;
    Tensor<float>* out = (Tensor<float>*)output;
    if (onedim_kernel) {
      int32_t filt_rad = ((int32_t)kernel_norm_->size()[0] - 1) / 2;

      // Perform horizontal filter pass
      cl_context->useKernel(kernel.c_str(), "SpatialDivisiveNormalizationHoriz");
      cl_context->setArg(0, in.storage());
      cl_context->setArg(1, std_pass1_->storage());
      cl_context->setArg(2, kernel_norm_->storage());
      cl_context->setArg(3, filt_rad);
      cl_context->runKernel(jtorch::deviceid, std_pass1_->dim(), 
        std_pass1_->size(), false);

      // Perform vertical filter pass
      cl_context->useKernel(kernel.c_str(), "SpatialDivisiveNormalizationVert");
      cl_context->setArg(0, std_pass1_->storage());
      cl_context->setArg(1, std_pass2_->storage());
      cl_context->setArg(2, kernel_norm_->storage());
      cl_context->setArg(3, filt_rad);
      cl_context->runKernel(jtorch::deviceid, std_pass2_->dim(), 
        std_pass2_->size(), false);
    } else {
      int32_t filt_rad_u = ((int32_t)kernel_norm_->size()[0] - 1) / 2;
      int32_t filt_rad_v = ((int32_t)kernel_norm_->size()[1] - 1) / 2;

      // Perform vertical filter pass
      cl_context->useKernel(kernel.c_str(), "SpatialDivisiveNormalization2D");
      cl_context->setArg(0, in.storage());
      cl_context->setArg(1, std_pass2_->storage());
      cl_context->setArg(2, kernel_norm_->storage());
      cl_context->setArg(3, filt_rad_u);
      cl_context->setArg(4, filt_rad_v);
      cl_context->runKernel(jtorch::deviceid, std_pass2_->dim(), 
        std_pass2_->size(), false);
    }

    // Perform accumulation and division pass
    cl_context->useKernel(kernel.c_str(), "SpatialDivisiveNormalizationAccumDiv");
    cl_context->setArg(0, std_pass2_->storage());
    cl_context->setArg(1, std_->storage());
    cl_context->setArg(2, std_coef_->storage());
    cl_context->setArg(3, (int)out->size()[2]);
    cl_context->setArg(4, threshold_);
    cl_context->runKernel(jtorch::deviceid, std_->dim(), std_->size(), false);

    // Perform normalization pass
    cl_context->useKernel(kernel.c_str(), "SpatialDivisiveNormalization");
    cl_context->setArg(0, in.storage());
    cl_context->setArg(1, out->storage());
    cl_context->setArg(2, std_->storage());
    cl_context->runKernel(jtorch::deviceid, out->dim(), out->size(), false);
  }

  TorchStage* SpatialDivisiveNormalization::loadFromFile(std::ifstream& file) {
    // This whole thing is a little wasteful.  I copy to GPU here, and then
    // I copy it back down in the constructor anyway...  But it's good enough
    // for now.
    int32_t kernel_size_2, kernel_size_1;  // kernel_size_1 is the inner dim
    file.read((char*)(&kernel_size_1), sizeof(kernel_size_1));
    file.read((char*)(&kernel_size_2), sizeof(kernel_size_2));
    Tensor<float>* kernel;
    if (kernel_size_2 > 1) {
      // The kernel is 2D
      uint32_t dim = 2;
      uint32_t size[2] = {kernel_size_1, kernel_size_2};
      kernel = new Tensor<float>(dim, size);
    } else {
      uint32_t dim = 1;
      uint32_t size[1] = {kernel_size_1};
      kernel = new Tensor<float>(dim, size);
    }
    float* kernel_cpu = new float[kernel->nelems()];
    file.read((char*)(kernel_cpu), kernel->nelems() * sizeof(*kernel_cpu));
    kernel->setData(kernel_cpu);
    float threshold;
    file.read((char*)(&threshold), sizeof(threshold));
    TorchStage* ret = new SpatialDivisiveNormalization(*kernel, threshold);
    delete kernel;
    delete[] kernel_cpu;
    return ret;
  }

}  // namespace jtorch