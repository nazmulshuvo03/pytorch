#include <gtest/gtest.h>

#include <ATen/ATen.h>

#include <iostream>
using namespace std;
using namespace at;

void trace() {
  Tensor foo = rand({12, 12});

  // ASSERT foo is 2-dimensional and holds floats.
  auto foo_a = foo.accessor<float, 2>();
  float trace = 0;

  for (int i = 0; i < foo_a.size(0); i++) {
    trace += foo_a[i][i];
  }

  ASSERT_FLOAT_EQ(foo.trace().item<float>(), trace);
}

TEST(atest, operators) {
  int a = 0b10101011;
  int b = 0b01111011;

  auto a_tensor = tensor({a});
  auto b_tensor = tensor({b});

  ASSERT_TRUE(tensor({~a}).equal(~a_tensor));
  ASSERT_TRUE(tensor({a | b}).equal(a_tensor | b_tensor));
  ASSERT_TRUE(tensor({a & b}).equal(a_tensor & b_tensor));
  ASSERT_TRUE(tensor({a ^ b}).equal(a_tensor ^ b_tensor));
}

template <typename T>
void run_logical_op_test(const Tensor& exp, T func) {
  auto x_tensor = tensor({1, 1, 0, 1, 0});
  auto y_tensor = tensor({0, 1, 0, 1, 1});
  // Test op over integer tensors
  auto out_tensor = empty({5}, kInt);
  func(out_tensor, x_tensor, y_tensor);
  ASSERT_EQ(out_tensor.dtype(), kInt);
  ASSERT_TRUE(exp.equal(out_tensor));
  // Test op over boolean tensors
  out_tensor = empty({5}, kBool);
  func(out_tensor, x_tensor.to(kBool), y_tensor.to(kBool));
  ASSERT_EQ(out_tensor.dtype(), kBool);
  ASSERT_TRUE(out_tensor.equal(exp.to(kBool)));
}

TEST(atest, logical_and_operators) {
  run_logical_op_test(tensor({0, 1, 0, 1, 0}), logical_and_out);
}
TEST(atest, logical_or_operators) {
  run_logical_op_test(tensor({1, 1, 0, 1, 1}), logical_or_out);
}
TEST(atest, logical_xor_operators) {
  run_logical_op_test(tensor({1, 0, 0, 0, 1}), logical_xor_out);
}

TEST(atest, lt_operators) {
  run_logical_op_test<
      at::Tensor& (*)(at::Tensor&, const at::Tensor&, const at::Tensor&)>(
      tensor({0, 0, 0, 0, 1}), lt_out);
}

TEST(atest, le_operators) {
  run_logical_op_test<
      at::Tensor& (*)(at::Tensor&, const at::Tensor&, const at::Tensor&)>(
      tensor({0, 1, 1, 1, 1}), le_out);
}

TEST(atest, gt_operators) {
  run_logical_op_test<
      at::Tensor& (*)(at::Tensor&, const at::Tensor&, const at::Tensor&)>(
      tensor({1, 0, 0, 0, 0}), gt_out);
}

TEST(atest, ge_operators) {
  run_logical_op_test<
      at::Tensor& (*)(at::Tensor&, const at::Tensor&, const at::Tensor&)>(
      tensor({1, 1, 1, 1, 0}), ge_out);
}

TEST(atest, eq_operators) {
  run_logical_op_test<
      at::Tensor& (*)(at::Tensor&, const at::Tensor&, const at::Tensor&)>(
      tensor({0, 1, 1, 1, 0}), eq_out);
}

TEST(atest, ne_operators) {
  run_logical_op_test<
      at::Tensor& (*)(at::Tensor&, const at::Tensor&, const at::Tensor&)>(
      tensor({1, 0, 0, 0, 1}), ne_out);
}

// TEST_CASE( "atest", "[]" ) {
TEST(atest, atest) {
  manual_seed(123);

  auto foo = rand({12, 6});

  ASSERT_EQ(foo.size(0), 12);
  ASSERT_EQ(foo.size(1), 6);

  foo = foo + foo * 3;
  foo -= 4;

  Scalar a = 4;
  float b = a.to<float>();
  ASSERT_EQ(b, 4);

  foo = ((foo * foo) == (foo.pow(3))).to(kByte);
  foo = 2 + (foo + 1);
  // foo = foo[3];
  auto foo_v = foo.accessor<uint8_t, 2>();

  for (int i = 0; i < foo_v.size(0); i++) {
    for (int j = 0; j < foo_v.size(1); j++) {
      foo_v[i][j]++;
    }
  }

  ASSERT_TRUE(foo.equal(4 * ones({12, 6}, kByte)));

  trace();

  float data[] = {1, 2, 3, 4, 5, 6};

  auto f = from_blob(data, {1, 2, 3});
  auto f_a = f.accessor<float, 3>();

  ASSERT_EQ(f_a[0][0][0], 1.0);
  ASSERT_EQ(f_a[0][1][1], 5.0);

  ASSERT_EQ(f.strides()[0], 6);
  ASSERT_EQ(f.strides()[1], 3);
  ASSERT_EQ(f.strides()[2], 1);
  ASSERT_EQ(f.sizes()[0], 1);
  ASSERT_EQ(f.sizes()[1], 2);
  ASSERT_EQ(f.sizes()[2], 3);

  // TODO(ezyang): maybe do a more precise exception type.
  ASSERT_THROW(f.resize_({3, 4, 5}), std::exception);
  {
    int isgone = 0;
    {
      auto f2 = from_blob(data, {1, 2, 3}, [&](void*) { isgone++; });
    }
    ASSERT_EQ(isgone, 1);
  }
  {
    int isgone = 0;
    Tensor a_view;
    {
      auto f2 = from_blob(data, {1, 2, 3}, [&](void*) { isgone++; });
      a_view = f2.view({3, 2, 1});
    }
    ASSERT_EQ(isgone, 0);
    a_view.reset();
    ASSERT_EQ(isgone, 1);
  }

  if (at::hasCUDA()) {
    int isgone = 0;
    {
      auto base = at::empty({1, 2, 3}, TensorOptions(kCUDA));
      auto f2 = from_blob(base.data_ptr(), {1, 2, 3}, [&](void*) { isgone++; });
    }
    ASSERT_EQ(isgone, 1);

    // Attempt to specify the wrong device in from_blob
    auto t = at::empty({1, 2, 3}, TensorOptions(kCUDA, 0));
    EXPECT_ANY_THROW(from_blob(t.data_ptr(), {1, 2, 3}, at::Device(kCUDA, 1)));

    // Infers the correct device
    auto t_ = from_blob(t.data_ptr(), {1, 2, 3}, kCUDA);
    ASSERT_EQ(t_.device(), at::Device(kCUDA, 0));
  }
}
