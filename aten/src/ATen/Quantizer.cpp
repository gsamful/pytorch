#include <ATen/ATen.h>
#include <ATen/Quantizer.h>
#include <ATen/QTensorImpl.h>
#include <ATen/NativeFunctions.h>
#include <ATen/native/TensorFactories.h>

namespace at {

std::shared_ptr<Quantizer> make_per_layer_affine_quantizer(double scale, int64_t zero_point) {
  return std::make_shared<PerLayerAffineQuantizer>(static_cast<float>(scale), static_cast<uint8_t>(zero_point));
}

inline QTensorImpl* get_qtensorimpl(const QTensor& self) {
  // TODO: remove this when Variable and Tensor are merged
  AT_ASSERTM(!self.is_variable(), "_internal_get_QTensorImpl: should not be a variable");
  // TODO: uncomment after is_quantized() is implmeented
  // AT_ASSERTM(self.is_quantized(), "_internal_get_QTensorImpl: not a quantized tensor");
  return static_cast<QTensorImpl*>(self.unsafeGetTensorImpl());
}

inline QTensor new_qtensor(
    IntArrayRef sizes, const TensorOptions& options, float scale, int32_t zero_point) {
  AT_ASSERT(options.device().is_cpu());

  native::check_size_nonnegative(sizes);
  auto* allocator = at::getCPUAllocator();
  int64_t nelements = at::prod_intlist(sizes);
  auto dtype = options.dtype();
  AT_ASSERT(isQIntType(dtype));
  auto storage_impl = c10::make_intrusive<StorageImpl>(
    dtype,
    nelements,
    allocator->allocate(nelements * dtype.itemsize()),
    allocator,
    /*resizable=*/true);
  auto quantizer = make_per_layer_affine_quantizer(scale, zero_point);
  auto tensor = detail::make_tensor<QTensorImpl>(
      storage_impl, at::CPUTensorId(), false, std::move(quantizer));
  // Default TensorImpl has size [0]
  if (sizes.size() != 1 || sizes[0] != 0) {
    get_qtensorimpl(tensor)->set_sizes_contiguous(sizes);
  }
  return tensor;
}

qint8 QuantizeUint8(float scale, int32_t zero_point, float value) {
  const int32_t qmin = std::numeric_limits<uint8_t>::min();
  const int32_t qmax = std::numeric_limits<uint8_t>::max();

  auto r = zero_point + static_cast<int32_t>(Round(value / scale));
  r = std::max(r, qmin);
  r = std::min(r, qmax);
  return static_cast<qint8>(r);
}

QTensor PerLayerAffineQuantizer::quantize(RealTensor tensor) {
  IntArrayRef sizes = tensor.sizes();
  QTensor qv = new_qtensor(sizes, tensor.options().dtype(at::kQInt8), scale_, zero_point_);
  auto qvd = qv.data<qint8>();
  const float* svd = tensor.data<float>();
  for (int i = 0; i < tensor.numel(); ++i) {
    qvd[i] = QuantizeUint8(scale_, zero_point_, svd[i]);
  }
  return qv;
}

RealTensor PerLayerAffineQuantizer::dequantize(QTensor tensor) {
  std::vector<int64_t> sizes = tensor.sizes().vec();
  at::TensorOptions real_options = tensor.options().dtype(at::kFloat);

  RealTensor rv = at::empty(sizes, real_options);
  const auto* qvd = tensor.data<qint8>();
  float* rvd = rv.data<float>();
  for (auto i = 0; i < tensor.numel(); ++i) {
    rvd[i] = (static_cast<uint32_t>(qvd[i].val_) - zero_point_) * scale_;
  }
  return rv;
}

} // namespace at
