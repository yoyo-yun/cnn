#pragma once

#include "cnn/cnn.h"
#include "cnn/nodes.h"

namespace cnn { namespace expr {

typedef struct Expression{
  ComputationGraph *pg;
  VariableIndex i;

  Expression() : pg(nullptr) { }
  Expression(ComputationGraph *pg, VariableIndex i) : pg(pg), i(i) { }
  const Tensor& value() { return pg->get_value(i); }
} ;

Expression input(ComputationGraph& g, cnn::real s);
Expression input(ComputationGraph& g, const cnn::real *ps);
Expression input(ComputationGraph& g, const Dim& d, const std::vector<cnn::real>& pdata);
Expression input(ComputationGraph& g, const Dim& d, const std::vector<cnn::real>* pdata);
Expression input(ComputationGraph& g, const Dim& d, const cnn::real* pdata);
Expression reference(ComputationGraph& g, const Dim& d, const cnn::real* pdata);
Expression parameter(ComputationGraph& g, Parameters* p);
Expression lookup(ComputationGraph& g, LookupParameters* p, unsigned index);
Expression lookup(ComputationGraph& g, LookupParameters* p, const unsigned* pindex);
Expression const_lookup(ComputationGraph& g, LookupParameters* p, unsigned index);
Expression const_lookup(ComputationGraph& g, LookupParameters* p, const unsigned* pindex);
// Batched versions of lookup and const_lookup
Expression lookup(ComputationGraph& g, LookupParameters* p, const std::vector<unsigned>& indices);
Expression lookup(ComputationGraph& g, LookupParameters* p, const std::vector<unsigned>* pindices);
Expression const_lookup(ComputationGraph& g, LookupParameters* p, const std::vector<unsigned>& indices);
Expression const_lookup(ComputationGraph& g, LookupParameters* p, const std::vector<unsigned>* pindices);
Expression zeroes(ComputationGraph& g, const Dim& d);

Expression operator-(const Expression& x);
Expression operator+(const Expression& x, const Expression& y);
Expression operator+(const Expression& x, cnn::real y);
Expression operator+(cnn::real x, const Expression& y);
Expression operator-(const Expression& x, const Expression& y);
Expression operator-(cnn::real x, const Expression& y);
Expression operator-(const Expression& x, cnn::real y);
Expression operator*(const Expression& x, const Expression& y);
Expression operator*(const Expression& x, cnn::real y);
inline Expression operator*(cnn::real y, const Expression& x) { return x * y; }
inline Expression operator/(const Expression& x, cnn::real y) { return x * (1.f / y); }
// componentwise division
Expression cdiv(const Expression& x, const Expression& y);
Expression colwise_add(const Expression& x, const Expression& bias);
// z_ij = x_ijk * y_k
Expression contract3d_1d(const Expression& x, const Expression& y);
// z_ij = x_ijk * y_k + b_ij
Expression contract3d_1d(const Expression& x, const Expression& y, const Expression& b);

Expression sqrt(const Expression& x);
// something wrong in Eigen to evaluate erf
//Expression erf(const Expression& x);
Expression tanh(const Expression& x);
Expression exp(const Expression& x);
Expression square(const Expression& x);
Expression cube(const Expression& x);
//Expression lgamma(const Expression& x);
Expression log(const Expression& x);
Expression logistic(const Expression& x);
Expression rectify(const Expression& x);
Expression exponential_linear_units(const Expression& x, cnn::real scale = 1.0);
Expression hinge(const Expression& x, unsigned index, cnn::real m = 1.0);
Expression hinge(const Expression& x, const unsigned* pindex, cnn::real m = 1.0);
Expression log_softmax(const Expression& x);
Expression log_softmax(const Expression& x, const std::vector<unsigned>& restriction);
Expression softmax(const Expression& x);
Expression softsign(const Expression& x);
Expression pow(const Expression& x, const Expression& y);
Expression min(const Expression& x, const Expression& y);
Expression max(const Expression& x, const Expression& y);
Expression noise(const Expression& x, cnn::real stddev);
Expression dropout(const Expression& x, cnn::real p);
Expression block_dropout(const Expression& x, cnn::real p);

Expression reshape(const Expression& x, const Dim& d);
Expression transpose(const Expression& x);

Expression trace_of_product(const Expression& x, const Expression& y);
Expression cwise_multiply(const Expression& x, const Expression& y);

Expression dot_product(const Expression& x, const Expression& y);
Expression squared_distance(const Expression& x, const Expression& y);
Expression huber_distance(const Expression& x, const Expression& y, cnn::real c = 1.345f);
Expression l1_distance(const Expression& x, const Expression& y);
Expression binary_log_loss(const Expression& x, const Expression& y);
Expression pairwise_rank_loss(const Expression& x, const Expression& y, cnn::real m=1.0);
Expression poisson_loss(const Expression& x, unsigned y);
Expression poisson_loss(const Expression& x, const unsigned* py);

// reduce to a scalar
// to-do reduce to a vector given a dimension
Expression reduce(const Expression& x); 

// various convolutiony things
Expression conv1d_narrow(const Expression& x, const Expression& f);
Expression conv1d_wide(const Expression& x, const Expression& f);
Expression kmax_pooling(const Expression& x, unsigned k);
Expression fold_rows(const Expression& x, unsigned nrows=2);
Expression sum_cols(const Expression& x);
Expression kmh_ngram(const Expression& x, unsigned n);

// Sum the results of multiple batches
Expression sum_batches(const Expression& x);

// pick parts out of bigger objects
Expression pick(const Expression& x, unsigned v);
Expression pick(const Expression& x, unsigned* pv);
Expression pickrange(const Expression& x, unsigned v, unsigned u);
Expression columnslices(const Expression& x, unsigned row, unsigned start_column, unsigned exclusive_end_column);
//Expression pickneglogsoftmax(const Expression& x, unsigned v);
//Expression pickneglogsoftmax(const Expression& x, const std::vector<unsigned> & v);

namespace detail {
  template <typename F, typename T>
  Expression f(const T& xs) {
    ComputationGraph *pg = xs.begin()->pg;
    std::vector<VariableIndex> xis(xs.size());
    int i = 0;
    for (auto xi = xs.begin(); xi != xs.end(); ++xi) xis[i++] = xi->i;
    return Expression(pg, pg->add_function<F>(xis));
  }
}

template <typename T>
inline Expression logsumexp(const T& xs) { return detail::f<LogSumExp>(xs); }
inline Expression logsumexp(const std::initializer_list<Expression>& xs) { return detail::f<LogSumExp>(xs); }

template <typename T>
inline Expression sum(const T& xs) { return detail::f<Sum>(xs); }
inline Expression sum(const std::initializer_list<Expression>& xs) { return detail::f<Sum>(xs); }

template <typename T>
inline Expression max(const T& xs) { return detail::f<Max>(xs); }
inline Expression max(const std::initializer_list<Expression>& xs) { return detail::f<Max>(xs); }

template <typename T>
inline Expression average(const T& xs) { return detail::f<Average>(xs); }
inline Expression average(const std::initializer_list<Expression>& xs) { return detail::f<Average>(xs); }

template <typename T>
inline Expression concatenate_cols(const T& xs) { return detail::f<ConcatenateColumns>(xs); }
inline Expression concatenate_cols(const std::initializer_list<Expression>& xs) { return detail::f<ConcatenateColumns>(xs); }

template <typename T>
inline Expression concatenate(const T& xs) { return detail::f<Concatenate>(xs); }
inline Expression concatenate(const std::initializer_list<Expression>& xs) { return detail::f<Concatenate>(xs); }

template <typename T>
inline Expression affine_transform(const T& xs) { return detail::f<AffineTransform>(xs); }
inline Expression affine_transform(const std::initializer_list<Expression>& xs) { return detail::f<AffineTransform>(xs); }


} }

