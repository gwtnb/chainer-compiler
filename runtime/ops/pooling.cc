#include <math.h>

#include <algorithm>

#include <chainerx/routines/creation.h>
#include <chainerx/routines/manipulation.h>
#include <chainerx/routines/math.h>
#include <chainerx/routines/pooling.h>
#include <chainerx/routines/statistics.h>

#include <common/log.h>
#include <runtime/chainerx_util.h>
#include <runtime/gen_xcvm_ops.h>
#include <runtime/xcvm_state.h>

namespace chainer_compiler {
namespace runtime {

namespace {

template <class T>
class BackwardContext : public XCVMOpaque {
public:
    explicit BackwardContext(std::unique_ptr<T>&& fb) : fb_(std::move(fb)) {
    }
    virtual ~BackwardContext() = default;

    T* fb() const {
        return fb_.get();
    }

private:
    std::unique_ptr<T> fb_;
};

}  // namespace

std::tuple<chainerx::Array, XCVMOpaque*> MaxPoolOp::RunImpl(XCVMState* st, const chainerx::Array& x) {
    // TODO(hamaji): Revive CheckPoolInputs.
    std::unique_ptr<chainerx::MaxPoolForwardBackward> fb(
            x.device().GetMaxPoolForwardBackward(kernel_shape, ComplementStride(strides, x), ComplementPad(pads, x), cover_all));
    chainerx::Array out = fb->Forward(x);
    XCVMOpaque* ctx = new BackwardContext<chainerx::MaxPoolForwardBackward>(std::move(fb));
    return std::tie(out, ctx);
}

std::tuple<chainerx::Array, XCVMOpaque*> AveragePoolOp::RunImpl(XCVMState* st, const chainerx::Array& x) {
    // TODO(hamaji): Revive CheckPoolInputs.
    chainerx::AveragePoolPadMode pad_mode = count_include_pad ? chainerx::AveragePoolPadMode::kZero : chainerx::AveragePoolPadMode::kIgnore;
    std::unique_ptr<chainerx::AveragePoolForwardBackward> fb(
            x.device().GetAveragePoolForwardBackward(kernel_shape, ComplementStride(strides, x), ComplementPad(pads, x), pad_mode));
    chainerx::Array out = fb->Forward(x);
    XCVMOpaque* ctx = new BackwardContext<chainerx::AveragePoolForwardBackward>(std::move(fb));
    return std::tie(out, ctx);
}

chainerx::Array MaxPoolGradOp::RunImpl(XCVMState* st, const chainerx::Array& gy, const XCVMOpaque& ctx) {
    auto& context = dynamic_cast<const BackwardContext<chainerx::MaxPoolForwardBackward>&>(ctx);
    return context.fb()->Backward(gy);
}

chainerx::Array AveragePoolGradOp::RunImpl(XCVMState* st, const chainerx::Array& gy, const XCVMOpaque& ctx) {
    auto& context = dynamic_cast<const BackwardContext<chainerx::AveragePoolForwardBackward>&>(ctx);
    return context.fb()->Backward(gy);
}

chainerx::Array MaxPoolGradNoCtxOp::RunImpl(XCVMState* st, const chainerx::Array& x, const chainerx::Array& y, const chainerx::Array& gy) {
    std::unique_ptr<chainerx::MaxPoolForwardBackward> fb(
            x.device().GetMaxPoolForwardBackward(kernel_shape, ComplementStride(strides, x), ComplementPad(pads, x), cover_all));
    fb->Forward(x);
    return fb->Backward(gy);
}

chainerx::Array AveragePoolGradNoCtxOp::RunImpl(
        XCVMState* st, const chainerx::Array& x, const chainerx::Array& y, const chainerx::Array& gy) {
    chainerx::AveragePoolPadMode pad_mode = count_include_pad ? chainerx::AveragePoolPadMode::kZero : chainerx::AveragePoolPadMode::kIgnore;
    std::unique_ptr<chainerx::AveragePoolForwardBackward> fb(
            x.device().GetAveragePoolForwardBackward(kernel_shape, ComplementStride(strides, x), ComplementPad(pads, x), pad_mode));
    fb->Forward(x);
    return fb->Backward(gy);
}

// A faithful re-implementation of Chainer's ROI ops.
// TODO(hamaji): Move this to ChainerX.
namespace {

chainerx::Slice ROIPoolingSlice(double size, double stride, double max_size, double roi_offset) {
    int64_t start = int64_t(floor(size * stride));
    int64_t end = int64_t(ceil((size + 1) * stride));
    start = std::min<double>(std::max<double>(start + roi_offset, 0), max_size);
    end = std::min<double>(std::max<double>(end + roi_offset, 0), max_size);
    return chainerx::Slice(start, end);
}

template <class ReduceFn>
chainerx::Array ROIPool2D(
        const chainerx::Array& bottom_data,
        const chainerx::Array& bottom_rois,
        const chainerx::Array& bottom_roi_indices,
        const Int64StackVector& output_shape,
        const float spatial_scale,
        ReduceFn fn) {
    CHECK_EQ(4, bottom_data.ndim());
    CHECK_EQ(2, output_shape.size());
    const int64_t channels = bottom_data.shape()[1];
    const int64_t height = bottom_data.shape()[2];
    const int64_t width = bottom_data.shape()[3];
    const int64_t n_rois = bottom_rois.shape()[0];
    const int64_t outh = output_shape[0];
    const int64_t outw = output_shape[1];
    chainerx::Array top_data = chainerx::Zeros(chainerx::Shape{n_rois, channels, outh, outw}, bottom_rois.dtype());

    for (int64_t i_roi = 0; i_roi < n_rois; ++i_roi) {
        int64_t idx = int64_t(chainerx::AsScalar(bottom_roi_indices.At({i_roi})));
        int64_t ymin = round(double(chainerx::AsScalar(bottom_rois.At({i_roi, 0})) * spatial_scale));
        int64_t xmin = round(double(chainerx::AsScalar(bottom_rois.At({i_roi, 1})) * spatial_scale));
        int64_t ymax = round(double(chainerx::AsScalar(bottom_rois.At({i_roi, 2})) * spatial_scale));
        int64_t xmax = round(double(chainerx::AsScalar(bottom_rois.At({i_roi, 3})) * spatial_scale));
        int64_t roi_height = std::max<int64_t>(ymax - ymin, 1);
        int64_t roi_width = std::max<int64_t>(xmax - xmin, 1);
        double strideh = 1. * roi_height / outh;
        double stridew = 1. * roi_width / outw;

        for (int64_t outy = 0; outy < outh; ++outy) {
            const chainerx::Slice& sliceh = ROIPoolingSlice(outy, strideh, height, ymin);
            if (sliceh.stop() <= sliceh.start()) {
                continue;
            }

            for (int64_t outx = 0; outx < outw; ++outx) {
                const chainerx::Slice& slicew = ROIPoolingSlice(outx, stridew, width, xmin);
                if (slicew.stop() <= slicew.start()) {
                    continue;
                }

                chainerx::Array roi_data = bottom_data.At({idx, chainerx::Slice(), sliceh, slicew}).Reshape({channels, -1});
                top_data.At({i_roi, chainerx::Slice(), outy, outx}) += fn(roi_data, 1, false);
            }
        }
    }

    return top_data;
}

nonstd::optional<std::tuple<double, int64_t, int64_t>> get_bounds(double p, int64_t limit) {
    if (p < -1 || limit < p) {
        return nonstd::nullopt;
    }
    if (p < 0.0) {
        p = 0.0;
    }
    int64_t low = static_cast<int64_t>(p);
    int64_t high;
    if (limit - 1 <= low) {
        p = high = low = limit - 1;
    } else {
        high = low + 1;
    }
    return nonstd::make_optional(std::make_tuple(p, low, high));
}

std::tuple<double, double, double, double> get_bilinear_interp_params(
        double y, double x, int64_t y_low, int64_t x_low, int64_t y_high, int64_t x_high) {
    double ly = y - y_low;
    double lx = x - x_low;
    double hy = 1.0 - ly;
    double hx = 1.0 - lx;

    double w1 = hy * hx;
    double w2 = hy * lx;
    double w3 = ly * hx;
    double w4 = ly * lx;
    return std::make_tuple(w1, w2, w3, w4);
}

template <class ReduceMode>
chainerx::Array ROIAlign2D(
        const chainerx::Array& bottom_data,
        const chainerx::Array& bottom_rois,
        const chainerx::Array& bottom_roi_indices,
        const Int64StackVector& output_shape,
        const float spatial_scale,
        const chainerx::StackVector<int64_t, chainerx::kMaxNdim>& sampling_ratio) {
    CHECK_EQ(4, bottom_data.ndim());
    CHECK_EQ(2, output_shape.size());
    CHECK_EQ(2, sampling_ratio.size());

    const int64_t channels = bottom_data.shape()[1];
    const int64_t height = bottom_data.shape()[2];
    const int64_t width = bottom_data.shape()[3];
    const int64_t n_rois = bottom_rois.shape()[0];
    const int64_t pooled_height = output_shape[0];
    const int64_t pooled_width = output_shape[1];
    chainerx::Array top_data = chainerx::Zeros(chainerx::Shape{n_rois, channels, pooled_height, pooled_width}, bottom_data.dtype());

    for (int64_t n = 0; n < n_rois; ++n) {
        int64_t roi_batch_ind = int64_t(chainerx::AsScalar(bottom_roi_indices.At({n})));
        double roi_start_h = double(chainerx::AsScalar(bottom_rois.At({n, 0})) * spatial_scale);
        double roi_start_w = double(chainerx::AsScalar(bottom_rois.At({n, 1})) * spatial_scale);
        double roi_end_h = double(chainerx::AsScalar(bottom_rois.At({n, 2})) * spatial_scale);
        double roi_end_w = double(chainerx::AsScalar(bottom_rois.At({n, 3})) * spatial_scale);
        double roi_height = std::max<double>(roi_end_h - roi_start_h, 1.);
        double roi_width = std::max<double>(roi_end_w - roi_start_w, 1.);
        double bin_size_h = roi_height / pooled_height;
        double bin_size_w = roi_width / pooled_width;

        int64_t roi_bin_grid_h = sampling_ratio[0];
        int64_t roi_bin_grid_w = sampling_ratio[1];

        for (int64_t c = 0; c < channels; ++c) {
            for (int64_t ph = 0; ph < pooled_height; ++ph) {
                for (int64_t pw = 0; pw < pooled_width; ++pw) {
                    ReduceMode reduce;
                    for (int64_t iy = 0; iy < roi_bin_grid_h; ++iy) {
                        double y = roi_start_h + ph * bin_size_h + (iy + 0.5) * bin_size_h / roi_bin_grid_h;
                        int64_t y_low, y_high;
                        auto y_bounds = get_bounds(y, height);
                        if (!y_bounds) {
                            continue;
                        }
                        std::tie(y, y_low, y_high) = *y_bounds;
                        for (int64_t ix = 0; ix < roi_bin_grid_w; ++ix) {
                            double x = roi_start_w + pw * bin_size_w + (ix + 0.5) * bin_size_w / roi_bin_grid_w;
                            int64_t x_low, x_high;
                            auto x_bounds = get_bounds(x, width);
                            if (!x_bounds) {
                                continue;
                            }
                            std::tie(x, x_low, x_high) = *x_bounds;

                            // bilinear interpolation {{
                            double w1, w2, w3, w4;
                            std::tie(w1, w2, w3, w4) = get_bilinear_interp_params(y, x, y_low, x_low, y_high, x_high);
                            auto v1 = float(chainerx::AsScalar(bottom_data.At({roi_batch_ind, c, y_low, x_low})));
                            auto v2 = float(chainerx::AsScalar(bottom_data.At({roi_batch_ind, c, y_low, x_high})));
                            auto v3 = float(chainerx::AsScalar(bottom_data.At({roi_batch_ind, c, y_high, x_low})));
                            auto v4 = float(chainerx::AsScalar(bottom_data.At({roi_batch_ind, c, y_high, x_high})));
                            double weighted_average = w1 * v1 + w2 * v2 + w3 * v3 + w4 * v4;
                            reduce.Reduce(weighted_average);
                            // }}
                        }
                    }
                    top_data.At({n, c, ph, pw}) += reduce.Finish(roi_bin_grid_h, roi_bin_grid_w);
                }
            }
        }
    }
    return top_data;
}

class ReduceByMax {
public:
    void Reduce(double weighted_average) {
        max_val_ = std::max(max_val_, weighted_average);
    }
    double Finish(int64_t /*roi_bin_grid_h*/, int64_t /*roi_bin_grid_w*/) const {
        return max_val_;
    }

private:
    double max_val_ = std::numeric_limits<double>::lowest();
};

class ReduceByAverage {
public:
    void Reduce(double weighted_average) {
        sum_ += weighted_average;
    }
    double Finish(int64_t roi_bin_grid_h, int64_t roi_bin_grid_w) const {
        return sum_ / (roi_bin_grid_h * roi_bin_grid_w);
    }

private:
    double sum_ = 0.0;
};

}  // namespace

chainerx::Array ROIMaxPool2DOp::RunImpl(
        XCVMState* st, const chainerx::Array& x, const chainerx::Array& rois, const chainerx::Array& roi_indices) {
    CHECK(!IsCudaDevice(&x.device())) << "Not implemented";
    return ROIPool2D(x, rois, roi_indices, output_shape, spatial_scale, chainerx::AMax);
}

chainerx::Array ROIAveragePool2DOp::RunImpl(
        XCVMState* st, const chainerx::Array& x, const chainerx::Array& rois, const chainerx::Array& roi_indices) {
    CHECK(!IsCudaDevice(&x.device())) << "Not implemented";
    return ROIPool2D(x, rois, roi_indices, output_shape, spatial_scale, chainerx::Mean);
}

chainerx::Array ROIMaxAlign2DOp::RunImpl(
        XCVMState* st, const chainerx::Array& x, const chainerx::Array& rois, const chainerx::Array& roi_indices) {
    CHECK(!IsCudaDevice(&x.device())) << "Not implemented";
    return ROIAlign2D<ReduceByMax>(x, rois, roi_indices, output_shape, spatial_scale, sampling_ratio);
}

chainerx::Array ROIAverageAlign2DOp::RunImpl(
        XCVMState* st, const chainerx::Array& x, const chainerx::Array& rois, const chainerx::Array& roi_indices) {
    CHECK(!IsCudaDevice(&x.device())) << "Not implemented";
    return ROIAlign2D<ReduceByAverage>(x, rois, roi_indices, output_shape, spatial_scale, sampling_ratio);
}

}  // namespace runtime
}  // namespace chainer_compiler
