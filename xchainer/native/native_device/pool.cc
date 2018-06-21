#include "xchainer/native/native_device.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <numeric>
#include <utility>

#include "xchainer/array.h"
#include "xchainer/constant.h"
#include "xchainer/dtype.h"
#include "xchainer/macro.h"
#include "xchainer/native/col2im.h"
#include "xchainer/native/elementwise.h"
#include "xchainer/native/im2col.h"
#include "xchainer/native/tensor_dot.h"
#include "xchainer/numeric_limits.h"
#include "xchainer/routines/connection.h"
#include "xchainer/routines/creation.h"
#include "xchainer/routines/indexing.h"
#include "xchainer/routines/math.h"
#include "xchainer/routines/pooling.h"
#include "xchainer/scalar.h"
#include "xchainer/shape.h"
#include "xchainer/stack_vector.h"

namespace xchainer {
namespace native {
namespace {

Scalar GetLowestOrInf(Dtype dtype) {
    return VisitDtype(dtype, [](auto pt) {
        using T = typename decltype(pt)::type;
        return Scalar{NumericLimits<T>::LowestOrInf()};
    });
}

// Returns axes that does the following transpose.
// (batch_size, channel, a_1, a_2, ...., a_n, b_1, b_2, ..., b_n) -> (batch_size, channel, b_1, b_2, ...., b_n, a_1, a_2, ..., a_n).
Axes GetSwapSpatialDimensionsAxes(size_t n) {
    Axes axes;
    axes.resize(2 + 2 * n);  // E.g. (batch_size, channel, out_1, out_2, ..., out_n, k_1, k_2, ..., k_n).
    axes[0] = 0;  // Batch dimension kept as is.
    axes[1] = 1;  // Channel dimension kept as is.
    for (size_t i = 2; i < n + 2; ++i) {  // Output and kernel spatial dimensions to be swapped.
        axes[i] = n + i;
        axes[n + i] = i;
    }
    return axes;
}

class NativeMaxPoolForwardBackward : public xchainer::MaxPoolForwardBackward {
public:
    explicit NativeMaxPoolForwardBackward(
            const StackVector<int64_t, kMaxNdim>& kernel_size,
            const StackVector<int64_t, kMaxNdim>& stride,
            const StackVector<int64_t, kMaxNdim>& pad,
            bool cover_all)
        : kernel_size_{kernel_size}, stride_{stride}, pad_{pad}, cover_all_{cover_all} {}

    Array Forward(const Array& x) override {
        // Convert to column representation of shape (batch_size, channel, k_1, k_2, ..., k_n, out_1, out_2, ..., out_n).
        col_ = internal::Im2Col(x.AsConstant(), kernel_size_, stride_, pad_, cover_all_, GetLowestOrInf(x.dtype()));
        axes_.resize(kernel_size_.size());
        std::iota(axes_.begin(), axes_.end(), 2);
        x_ = x.AsConstant();
        return col_.Max(axes_);
    }

    Array Backward(const Array& gout) override {
        indices_ = col_.ArgMax(axes_);
        assert(indices_.shape() == gout.shape());

        // Compute flattened col gradients.
        int64_t kernel_total_size = std::accumulate(kernel_size_.begin(), kernel_size_.end(), int64_t{1}, std::multiplies<>());
        int64_t out_total_size = indices_.GetTotalSize();
        Shape out_flat{out_total_size};
        Device& device = x_.device();
        Array gcol = Zeros({out_total_size * kernel_total_size}, x_.dtype(), device);
        offset_ = Arange(0, out_total_size * kernel_total_size, kernel_total_size, indices_.dtype(), device);
        device.AddAt(gcol, indices_.Reshape(out_flat) + offset_, {0}, gout.AsConstant().Reshape(out_flat), gcol);

        // Reshape col gradients to (batch_size, channel, out_1, out_2, ..., out_n, k_1, k_2, ..., k_n).
        Shape out_shape_with_kernel = gout.shape();
        std::copy(kernel_size_.begin(), kernel_size_.end(), std::back_inserter(out_shape_with_kernel));

        // Transform col gradients to input shape.
        return internal::Col2Im(
                gcol.Reshape(out_shape_with_kernel).Transpose(GetSwapSpatialDimensionsAxes(kernel_size_.size())),
                stride_,
                pad_,
                {x_.shape().begin() + 2, x_.shape().end()});
    }

    Array DoubleBackward(const Array& ggx) override {
        Array col = internal::Im2Col(ggx.AsConstant(), kernel_size_, stride_, pad_, cover_all_, GetLowestOrInf(x_.dtype()));
        return Take(
                col.Transpose(GetSwapSpatialDimensionsAxes(kernel_size_.size())).Reshape({col.GetTotalSize()}),
                indices_ + offset_.Reshape(indices_.shape()),
                0);
    }

private:
    const StackVector<int64_t, kMaxNdim> kernel_size_;
    const StackVector<int64_t, kMaxNdim> stride_;
    const StackVector<int64_t, kMaxNdim> pad_;
    Array x_;
    bool cover_all_;
    Array col_{};
    Axes axes_{};
    Array indices_{};
    Array offset_{};
};

}  // namespace

std::unique_ptr<MaxPoolForwardBackward> NativeDevice::GetMaxPoolForwardBackward(
        const StackVector<int64_t, kMaxNdim>& kernel_size,
        const StackVector<int64_t, kMaxNdim>& stride,
        const StackVector<int64_t, kMaxNdim>& pad,
        bool cover_all) {
    return std::make_unique<NativeMaxPoolForwardBackward>(kernel_size, stride, pad, cover_all);
}

namespace {

// TODO(hvy): Use Device::Mean when implemented.
void Mean(const Array& a, const Axes& axis, const Array& out) {
    Device& device = a.device();
    device.Sum(a, axis, out);
    device.DivideAS(out, xchainer::internal::CountItemsAlongAxes(a.shape(), axis), out);
}

Array GetPadModeIgnorePoolingWidths(
        const Shape& shape,
        const StackVector<int64_t, kMaxNdim>& kernel_size,
        const StackVector<int64_t, kMaxNdim>& stride,
        const StackVector<int64_t, kMaxNdim>& pad,
        Dtype dtype) {
    int8_t n = shape.ndim() - 2;
    assert(n == static_cast<int8_t>(kernel_size.size()));
    assert(n == static_cast<int8_t>(stride.size()));
    assert(n == static_cast<int8_t>(pad.size()));

    Array widths;
    for (int64_t i = 0; i < n; ++i) {
        int64_t dim_i = shape[2 + i];
        int64_t kernel_size_i = kernel_size[i];
        int64_t stride_i = stride[i];
        int64_t pad_i = pad[i];

        Array width = Empty({xchainer::internal::GetConvOutDim(dim_i, kernel_size_i, stride_i, pad_i, false)}, dtype);
        VisitDtype(dtype, [dim_i, kernel_size_i, stride_i, pad_i, &width](auto pt) {
            using T = typename decltype(pt)::type;
            struct Impl {
                void operator()(int64_t i, T& w) {
                    T start = i * s - p;
                    T end = start + k;
                    if (start < 0) {
                        start = 0;
                    }
                    if (end > d) {
                        end = d;
                    }
                    w = end - start;
                }

                T d;
                T k;
                T s;
                T p;
            };
            Elementwise<T>(
                    Impl{static_cast<T>(dim_i), static_cast<T>(kernel_size_i), static_cast<T>(stride_i), static_cast<T>(pad_i)}, width);
        });

        if (i == 0) {
            widths = std::move(width);
        } else {
            Shape widths_expanded = widths.shape();
            widths_expanded.emplace_back(1);

            Shape width_expanded{1};
            std::copy(width.shape().begin(), width.shape().end(), std::back_inserter(width_expanded));

            widths = TensorDot(widths.Reshape(widths_expanded), width.Reshape(width_expanded), {static_cast<int8_t>(widths.ndim())}, {0});
        }
    }
    return widths;
}

class NativeAveragePoolForwardBackward : public xchainer::AveragePoolForwardBackward {
public:
    explicit NativeAveragePoolForwardBackward(
            const StackVector<int64_t, kMaxNdim>& kernel_size,
            const StackVector<int64_t, kMaxNdim>& stride,
            const StackVector<int64_t, kMaxNdim>& pad,
            AveragePoolPadMode pad_mode)
        : kernel_size_{kernel_size}, stride_{stride}, pad_{pad}, pad_mode_{pad_mode} {}

    Array Forward(const Array& x) override {
        Array col = internal::Im2Col(x.AsConstant(), kernel_size_, stride_, pad_, false, 0);

        // Average along the kernel dimensions of col with shape (batch_size, channel, k_1, k_2, ..., k_n, out_1, out_2, ..., out_n).
        Axes kernel_axes;
        kernel_axes.resize(kernel_size_.size());
        std::iota(kernel_axes.begin(), kernel_axes.end(), 2);  // From k_1, up to k_n.

        Array out = xchainer::internal::EmptyReduced(col.shape(), col.dtype(), kernel_axes, false, col.device());

        switch (pad_mode_) {
            case AveragePoolPadMode::kZero:
                Mean(col, kernel_axes, out);
                break;
            case AveragePoolPadMode::kIgnore: {
                Device& device = x.device();
                device.Sum(col, kernel_axes, out);
                Array widths = GetPadModeIgnorePoolingWidths(x.shape(), kernel_size_, stride_, pad_, x.dtype()).BroadcastTo(out.shape());
                device.Divide(out, widths, out);
                break;
            }
            default:
                XCHAINER_NEVER_REACH();
        }
        return out;
    }

    Array Backward(const Array& /*gout*/) override { throw NotImplementedError{}; }

private:
    const StackVector<int64_t, kMaxNdim> kernel_size_;
    const StackVector<int64_t, kMaxNdim> stride_;
    const StackVector<int64_t, kMaxNdim> pad_;
    const AveragePoolPadMode pad_mode_;
};

}  // namespace

std::unique_ptr<AveragePoolForwardBackward> NativeDevice::GetAveragePoolForwardBackward(
        const StackVector<int64_t, kMaxNdim>& kernel_size,
        const StackVector<int64_t, kMaxNdim>& stride,
        const StackVector<int64_t, kMaxNdim>& pad,
        AveragePoolPadMode pad_mode) {
    return std::make_unique<NativeAveragePoolForwardBackward>(kernel_size, stride, pad, pad_mode);
}

}  // namespace native
}  // namespace xchainer
