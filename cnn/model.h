#pragma once

#include <vector>
#include <unordered_set>
#include <string>
#include <map>
#include <unordered_map>

#include <boost/serialization/split_member.hpp>
#include <boost/serialization/vector.hpp>

#include "cnn/tensor.h"
#include "cnn/cuda.h"
#include "cnn/macros.h"

namespace cnn {

// to deal with sparse updates, there are two parameter classes:
// * Parameters represents a vector, matrix, (eventually higher order tensors)
//   of parameters. These are densely updated.
// * LookupParameters represents a table of vectors that are used to embed a
//   set of discrete objects. These are sparsely updated.

struct ParametersBase {
  friend class Model;
  virtual void scale_parameters(cnn::real a) = 0;
  virtual void squared_l2norm(cnn::real* sqnorm) const = 0;
  virtual void g_squared_l2norm(cnn::real* sqnorm) const = 0;
  virtual   void g_simple_clipping(cnn::real threshold) = 0;

  virtual size_t size() const = 0;
  virtual ~ParametersBase();
};

// represents parameters (e.g., a weight matrix) that will be optimized
struct Parameters : public ParametersBase {
  friend class Model;
  void scale_parameters(cnn::real a) override;
  void reset_to_zero() ;
  void squared_l2norm(cnn::real* sqnorm) const override;
  void g_squared_l2norm(cnn::real* sqnorm) const override;
  void g_simple_clipping(cnn::real threshold);

  size_t size() const override;

  void copy(const Parameters & val);
  void accumulate_grad(const Tensor& g);
  void clear();

  Dim dim;
  Tensor values;
  Tensor g;
  std::string name;
private:
  Parameters() {}
  ~Parameters() {
      cnn_mm_free(values.v);
      cnn_mm_free(g.v); 
  }
  explicit Parameters(const Dim& d, cnn::real minmax, std::string nodename = ""); // initialize with ~U(-minmax,+minmax)
                                 // or Glorot initialization if minmax = 0
  friend class boost::serialization::access;
  template<class Archive> void serialize(Archive& ar, const unsigned int version) {
      ar & dim;
      ar & values;
  }
};

// represents a matrix/vector embedding of a discrete set
struct LookupParameters : public ParametersBase {
  friend class Model;
  void scale_parameters(cnn::real a) override;
  void squared_l2norm(cnn::real* sqnorm) const override;
  void g_squared_l2norm(cnn::real* sqnorm) const override;
  void g_simple_clipping(cnn::real threshold);
  size_t size() const override;
  void Initialize(unsigned index, const std::vector<cnn::real>& val);

  void copy(const LookupParameters & val);
  void copy(const std::map<int, std::vector<cnn::real>> & vWordEmbedding);
  void accumulate_grad(unsigned index, const Tensor& g);
  void clear();

  Dim dim;
  std::vector<Tensor> values;

  // working memory for those values and gradient that are actively used, they can be in GPU, where
  // main memory is in CPU
  std::unordered_map<unsigned, Tensor> values_for_non_zero_grads;
  std::unordered_map<unsigned, Tensor> grads;

  std::string name;

private:
  LookupParameters() { }
  ~LookupParameters();
  LookupParameters(unsigned n, const Dim& d, cnn::real scale, std::string nodename = "");

  /// free working copies of grads and values
  void free_working_copies();

  friend class boost::serialization::access;
  template<class Archive>
  void save(Archive& ar, const unsigned int) const {
    ar & dim;
    int nv = (int) values.size();
    ar & nv;
    for (unsigned i = 0; i < values.size(); ++i)
    {
        ar & values[i];
    }
  }
  template<class Archive>
  void load(Archive& ar, const unsigned int) {
    int nv; 
    ar & dim;
    ar & nv;
    assert(nv == (int)values.size());
    for (unsigned i = 0; i < values.size(); ++i)
    {
#ifdef USE_CPU_FOR_LOOKUP_PARAM
        values[i].m_device_id = CPUDEVICE;
#endif
        ar & values[i];
    }
  }
  BOOST_SERIALIZATION_SPLIT_MEMBER()
};

// this is a collection of parameters
// if you need a matrix of parameters, or a lookup table - ask an instance of this class
// this knows how to serialize itself
// parameters know how to track their gradients, but any extra information (like velocity) will live here
class Model {
private: 
    mutable cnn::real *gscale; /// gradient scale, memory to be allocated by GPU if HAVE_CUDA
    /// for speed-up, this memory is called from cudaMallocHost if HAVE_CUDA
 public:
    Model() : gradient_norm_scratch() { 
        gscale = nullptr; 
    }
    ~Model();

    /// for gradient clipping
    cnn::real gradient_l2_norm() const;
    /// clip gradients if their values are larger than the threshold
    void simple_gradient_clipping(cnn::real threshold);

    void reset_gradient();
    // set scale to use custom initialization
    Parameters* add_parameters(const Dim& d, cnn::real scale = 1.0f, std::string nodename = "");
    LookupParameters* add_lookup_parameters(unsigned n, const Dim& d, cnn::real scale = 1.0f, std::string nodename = "");
    // project weights so their L2 norm = radius
    void project_weights(cnn::real radius = 1.0f);

    const std::vector<ParametersBase*>& all_parameters_list() const { return all_params; }
    const std::vector<Parameters*>& parameters_list() const { return params; }
    const std::vector<LookupParameters*>& lookup_parameters_list() const { return lookup_params; }

 private:
    friend class boost::serialization::access;
    template<class Archive>
    void save(Archive& ar, const unsigned int version) const {
        int np = (int) params.size();
        int nlp = (int) lookup_params.size();
        ar & np;
        ar & nlp;
        for (unsigned i = 0; i < params.size(); ++i)
          ar & *params[i];
        for (unsigned i = 0; i < lookup_params.size(); ++i)
          ar & *lookup_params[i];
    }
    template<class Archive>
    void load(Archive& ar, const unsigned int ) {
        int np, nlp;
        ar & np;
        ar & nlp;
        assert(np == (int)params.size());
        assert(nlp == (int)lookup_params.size());
        for (unsigned i = 0; i < params.size(); ++i)
          ar & *params[i];
        for (unsigned i = 0; i < lookup_params.size(); ++i)
          ar & *lookup_params[i];
        all_params.clear();
        for (auto p : params) all_params.push_back(p);
        for (auto p : lookup_params) all_params.push_back(p);
    }
    BOOST_SERIALIZATION_SPLIT_MEMBER()

    std::vector<ParametersBase*> all_params;
    std::vector<Parameters*> params;
    std::vector<LookupParameters*> lookup_params;
    mutable cnn::real* gradient_norm_scratch;
};
void save_cnn_model(std::string filename, Model* model);
void load_cnn_model(std::string filename, Model* model);


} // namespace cnn

