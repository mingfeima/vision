#include <ATen/ATen.h>
#include <ATen/Parallel.h>
#include <torch/library.h>

#include "./roi_align_common.h"

namespace vision {
namespace ops {

namespace {

template <typename T>
inline void roi_align_single_framework_forward(
    const T* input,
    const int count,
    int channels,
    int height,
    int width,
    int pooled_height,
    int pooled_width,
    int roi_bin_grid_h,
    int roi_bin_grid_w,
    const std::vector<detail::PreCalc<T>>& pre_calc,
    T* output) {
  for (int c = 0; c < channels; c++) {
    const T* offset_input = input + c * height * width;
    int pre_calc_index = 0;

    for (int ph = 0; ph < pooled_height; ph++) {
      for (int pw = 0; pw < pooled_width; pw++) {
        int index = c * pooled_height * pooled_width + ph * pooled_width + pw;

        T output_val = 0.;
        for (int iy = 0; iy < roi_bin_grid_h; iy++) {
          for (int ix = 0; ix < roi_bin_grid_w; ix++) {
            detail::PreCalc<T> pc = pre_calc[pre_calc_index];
            output_val += pc.w1 * offset_input[pc.pos1] +
                    pc.w2 * offset_input[pc.pos2] +
                    pc.w3 * offset_input[pc.pos3] + pc.w4 * offset_input[pc.pos4];

            pre_calc_index += 1;
          }
        }
        output_val /= count; // Average pooling

        output[index] = output_val;
      } // for pw
    } // for ph
  } // for c
}

template <typename T>
inline void roi_align_single_framework_channels_last_forward(
    const T* input,
    const int count,
    int channels,
    int height,
    int width,
    int pooled_height,
    int pooled_width,
    int roi_bin_grid_h,
    int roi_bin_grid_w,
    const std::vector<detail::PreCalc<T>>& pre_calc,
    T* output) {
  // for 'normal' size of channels, should be L1 fit;
  // otherwise consider blocking on channels.
  int pre_calc_index = 0;
  for (int ph = 0; ph < pooled_height; ph++) {
    for (int pw = 0; pw < pooled_width; pw++) {
      T* out = output + (ph * pooled_width + pw) * channels;

      // pass I: do accumulation
      for (int iy = 0; iy < roi_bin_grid_h; iy++) {
        for (int ix = 0; ix < roi_bin_grid_w; ix++) {
          detail::PreCalc<T> pc = pre_calc[pre_calc_index];
          const T* in1 = input + pc.pos1 * channels;
          const T* in2 = input + pc.pos2 * channels;
          const T* in3 = input + pc.pos3 * channels;
          const T* in4 = input + pc.pos4 * channels;

          #pragma omp simd
          for (int c = 0; c < channels; c++) {
            out[c] += pc.w1 * in1[c] + pc.w2 * in2[c] + pc.w3 * in3[c] + pc.w4 * in4[c];
          }
          pre_calc_index += 1;
        }
      }

      // pass II: do average
      #pragma omp simd
      for (int c = 0; c < channels; c++) {
        out[c] /= count;
      }
    } // for pw
  } // for ph
}

template <typename T>
void roi_align_forward_kernel_impl(
    int n_rois,
    const T* input,
    const T& spatial_scale,
    int channels,
    int height,
    int width,
    int pooled_height,
    int pooled_width,
    int sampling_ratio,
    bool aligned,
    const T* rois,
    T* output,
    bool is_channels_last) {
  // (n, c, ph, pw) is an element in the pooled output
  // can be parallelized using omp
  at::parallel_for(0, n_rois, 1, [&](int begin, int end) {
    for (int n = begin; n < end; n++) {
      const T* offset_rois = rois + n * 5;
      int roi_batch_ind = offset_rois[0];

      // Do not using rounding; this implementation detail is critical
      T offset = aligned ? (T)0.5 : (T)0.0;
      T roi_start_w = offset_rois[1] * spatial_scale - offset;
      T roi_start_h = offset_rois[2] * spatial_scale - offset;
      T roi_end_w = offset_rois[3] * spatial_scale - offset;
      T roi_end_h = offset_rois[4] * spatial_scale - offset;

      T roi_width = roi_end_w - roi_start_w;
      T roi_height = roi_end_h - roi_start_h;
      if (!aligned) {
        // Force malformed ROIs to be 1x1
        roi_width = std::max(roi_width, (T)1.);
        roi_height = std::max(roi_height, (T)1.);
      }

      T bin_size_h = static_cast<T>(roi_height) / static_cast<T>(pooled_height);
      T bin_size_w = static_cast<T>(roi_width) / static_cast<T>(pooled_width);

      // We use roi_bin_grid to sample the grid and mimic integral
      int roi_bin_grid_h = (sampling_ratio > 0)
          ? sampling_ratio
          : ceil(roi_height / pooled_height); // e.g., = 2
      int roi_bin_grid_w =
          (sampling_ratio > 0) ? sampling_ratio : ceil(roi_width / pooled_width);

      // We do average (integral) pooling inside a bin
      // When the grid is empty, output zeros.
      const T count = std::max(roi_bin_grid_h * roi_bin_grid_w, 1); // e.g. = 4

      // we want to precalculate indices and weights shared by all channels,
      // this is the key point of optimization
      std::vector<detail::PreCalc<T>> pre_calc(
          roi_bin_grid_h * roi_bin_grid_w * pooled_width * pooled_height);
      detail::pre_calc_for_bilinear_interpolate(
          height,
          width,
          pooled_height,
          pooled_width,
          roi_start_h,
          roi_start_w,
          bin_size_h,
          bin_size_w,
          roi_bin_grid_h,
          roi_bin_grid_w,
          pre_calc);

      if (is_channels_last) {
        roi_align_single_framework_channels_last_forward(
            input + roi_batch_ind * height * width * channels,
            count,
            channels,
            height,
            width,
            pooled_height,
            pooled_width,
            roi_bin_grid_h,
            roi_bin_grid_w,
            pre_calc,
            output + n * pooled_width * pooled_height * channels);
      } else {
        roi_align_single_framework_forward(
            input + roi_batch_ind * channels * height * width,
            count,
            channels,
            height,
            width,
            pooled_height,
            pooled_width,
            roi_bin_grid_h,
            roi_bin_grid_w,
            pre_calc,
            output + n * channels * pooled_width * pooled_height);
      }
    } // for n
  });
}

template <typename T>
void bilinear_interpolate_gradient(
    int height,
    int width,
    T y,
    T x,
    T& w1,
    T& w2,
    T& w3,
    T& w4,
    int& x_low,
    int& x_high,
    int& y_low,
    int& y_high,
    int index /* index for debug only*/) {
  // deal with cases that inverse elements are out of feature map boundary
  if (y < -1.0 || y > height || x < -1.0 || x > width) {
    // empty
    w1 = w2 = w3 = w4 = 0.;
    x_low = x_high = y_low = y_high = -1;
    return;
  }

  if (y <= 0)
    y = 0;
  if (x <= 0)
    x = 0;

  y_low = (int)y;
  x_low = (int)x;

  if (y_low >= height - 1) {
    y_high = y_low = height - 1;
    y = (T)y_low;
  } else {
    y_high = y_low + 1;
  }

  if (x_low >= width - 1) {
    x_high = x_low = width - 1;
    x = (T)x_low;
  } else {
    x_high = x_low + 1;
  }

  T ly = y - y_low;
  T lx = x - x_low;
  T hy = 1. - ly, hx = 1. - lx;

  // reference in forward
  // T v1 = input[y_low * width + x_low];
  // T v2 = input[y_low * width + x_high];
  // T v3 = input[y_high * width + x_low];
  // T v4 = input[y_high * width + x_high];
  // T val = (w1 * v1 + w2 * v2 + w3 * v3 + w4 * v4);

  w1 = hy * hx, w2 = hy * lx, w3 = ly * hx, w4 = ly * lx;
}

template <class T>
inline void add(T* address, const T& val) {
  *address += val;
}

template <typename T>
void roi_align_backward_kernel_impl(
    int nthreads,
    const T* grad_output,
    const T& spatial_scale,
    int channels,
    int height,
    int width,
    int pooled_height,
    int pooled_width,
    int sampling_ratio,
    bool aligned,
    T* grad_input,
    const T* rois,
    int n_stride,
    int c_stride,
    int h_stride,
    int w_stride) {
  for (int index = 0; index < nthreads; index++) {
    // (n, c, ph, pw) is an element in the pooled output
    int pw = index % pooled_width;
    int ph = (index / pooled_width) % pooled_height;
    int c = (index / pooled_width / pooled_height) % channels;
    int n = index / pooled_width / pooled_height / channels;

    const T* offset_rois = rois + n * 5;
    int roi_batch_ind = offset_rois[0];

    // Do not using rounding; this implementation detail is critical
    T offset = aligned ? (T)0.5 : (T)0.0;
    T roi_start_w = offset_rois[1] * spatial_scale - offset;
    T roi_start_h = offset_rois[2] * spatial_scale - offset;
    T roi_end_w = offset_rois[3] * spatial_scale - offset;
    T roi_end_h = offset_rois[4] * spatial_scale - offset;

    T roi_width = roi_end_w - roi_start_w;
    T roi_height = roi_end_h - roi_start_h;
    if (!aligned) {
      // Force malformed ROIs to be 1x1
      roi_width = std::max(roi_width, (T)1.);
      roi_height = std::max(roi_height, (T)1.);
    }

    T bin_size_h = static_cast<T>(roi_height) / static_cast<T>(pooled_height);
    T bin_size_w = static_cast<T>(roi_width) / static_cast<T>(pooled_width);

    T* offset_grad_input =
        grad_input + ((roi_batch_ind * channels + c) * height * width);

    int output_offset = n * n_stride + c * c_stride;
    const T* offset_grad_output = grad_output + output_offset;
    const T grad_output_this_bin =
        offset_grad_output[ph * h_stride + pw * w_stride];

    // We use roi_bin_grid to sample the grid and mimic integral
    int roi_bin_grid_h = (sampling_ratio > 0)
        ? sampling_ratio
        : ceil(roi_height / pooled_height); // e.g., = 2
    int roi_bin_grid_w =
        (sampling_ratio > 0) ? sampling_ratio : ceil(roi_width / pooled_width);

    // We do average (integral) pooling inside a bin
    const T count = roi_bin_grid_h * roi_bin_grid_w; // e.g. = 4

    for (int iy = 0; iy < roi_bin_grid_h; iy++) {
      const T y = roi_start_h + ph * bin_size_h +
          static_cast<T>(iy + .5f) * bin_size_h /
              static_cast<T>(roi_bin_grid_h); // e.g., 0.5, 1.5
      for (int ix = 0; ix < roi_bin_grid_w; ix++) {
        const T x = roi_start_w + pw * bin_size_w +
            static_cast<T>(ix + .5f) * bin_size_w /
                static_cast<T>(roi_bin_grid_w);

        T w1, w2, w3, w4;
        int x_low, x_high, y_low, y_high;

        bilinear_interpolate_gradient(
            height,
            width,
            y,
            x,
            w1,
            w2,
            w3,
            w4,
            x_low,
            x_high,
            y_low,
            y_high,
            index);

        T g1 = grad_output_this_bin * w1 / count;
        T g2 = grad_output_this_bin * w2 / count;
        T g3 = grad_output_this_bin * w3 / count;
        T g4 = grad_output_this_bin * w4 / count;

        if (x_low >= 0 && x_high >= 0 && y_low >= 0 && y_high >= 0) {
          // atomic add is not needed for now since it is single threaded
          add(offset_grad_input + y_low * width + x_low, static_cast<T>(g1));
          add(offset_grad_input + y_low * width + x_high, static_cast<T>(g2));
          add(offset_grad_input + y_high * width + x_low, static_cast<T>(g3));
          add(offset_grad_input + y_high * width + x_high, static_cast<T>(g4));
        } // if
      } // ix
    } // iy
  } // for
}

at::Tensor roi_align_forward_kernel(
    const at::Tensor& input,
    const at::Tensor& rois,
    double spatial_scale,
    int64_t pooled_height,
    int64_t pooled_width,
    int64_t sampling_ratio,
    bool aligned) {
  TORCH_CHECK(input.device().is_cpu(), "input must be a CPU tensor");
  TORCH_CHECK(rois.device().is_cpu(), "rois must be a CPU tensor");
  TORCH_CHECK(rois.size(1) == 5, "rois must have shape as Tensor[K, 5]");

  at::TensorArg input_t{input, "input", 1}, rois_t{rois, "rois", 2};

  at::CheckedFrom c = "roi_align_forward_kernel";
  at::checkAllSameType(c, {input_t, rois_t});

  auto num_rois = rois.size(0);
  auto channels = input.size(1);
  auto height = input.size(2);
  auto width = input.size(3);

  auto memory_format = input.suggest_memory_format();
  bool is_channels_last = memory_format == at::MemoryFormat::ChannelsLast;
  at::Tensor output = at::empty({0}, input.options());
  output.resize_({num_rois, channels, pooled_height, pooled_width}, memory_format).zero_();

  if (output.numel() == 0)
    return output;

  auto input_ = input.contiguous(memory_format), rois_ = rois.contiguous();
  AT_DISPATCH_FLOATING_TYPES_AND_HALF(
      input.scalar_type(), "roi_align_forward_kernel", [&] {
        roi_align_forward_kernel_impl<scalar_t>(
            num_rois,
            input_.data_ptr<scalar_t>(),
            spatial_scale,
            channels,
            height,
            width,
            pooled_height,
            pooled_width,
            sampling_ratio,
            aligned,
            rois_.data_ptr<scalar_t>(),
            output.data_ptr<scalar_t>(),
            is_channels_last);
      });
  return output;
}

at::Tensor roi_align_backward_kernel(
    const at::Tensor& grad,
    const at::Tensor& rois,
    double spatial_scale,
    int64_t pooled_height,
    int64_t pooled_width,
    int64_t batch_size,
    int64_t channels,
    int64_t height,
    int64_t width,
    int64_t sampling_ratio,
    bool aligned) {
  TORCH_CHECK(grad.device().is_cpu(), "grad must be a CPU tensor");
  TORCH_CHECK(rois.device().is_cpu(), "rois must be a CPU tensor");

  at::TensorArg grad_t{grad, "grad", 1}, rois_t{rois, "rois", 2};

  at::CheckedFrom c = "roi_align_backward_kernel";
  at::checkAllSameType(c, {grad_t, rois_t});

  at::Tensor grad_input =
      at::zeros({batch_size, channels, height, width}, grad.options());

  // handle possibly empty gradients
  if (grad.numel() == 0) {
    return grad_input;
  }

  // get stride values to ensure indexing into gradients is correct.
  int n_stride = grad.stride(0);
  int c_stride = grad.stride(1);
  int h_stride = grad.stride(2);
  int w_stride = grad.stride(3);

  auto rois_ = rois.contiguous();
  AT_DISPATCH_FLOATING_TYPES_AND_HALF(
      grad.scalar_type(), "roi_align_backward_kernel", [&] {
        roi_align_backward_kernel_impl<scalar_t>(
            grad.numel(),
            grad.data_ptr<scalar_t>(),
            spatial_scale,
            channels,
            height,
            width,
            pooled_height,
            pooled_width,
            sampling_ratio,
            aligned,
            grad_input.data_ptr<scalar_t>(),
            rois_.data_ptr<scalar_t>(),
            n_stride,
            c_stride,
            h_stride,
            w_stride);
      });
  return grad_input;
}

} // namespace

TORCH_LIBRARY_IMPL(torchvision, CPU, m) {
  m.impl(
      TORCH_SELECTIVE_NAME("torchvision::roi_align"),
      TORCH_FN(roi_align_forward_kernel));
  m.impl(
      TORCH_SELECTIVE_NAME("torchvision::_roi_align_backward"),
      TORCH_FN(roi_align_backward_kernel));
}

} // namespace ops
} // namespace vision
