#include "cnn/training.h"
#include "cnn/data-util.h"
#include "cnn/gpu-ops.h"

namespace cnn {

using namespace std;

extern AlignedMemoryPool<ALIGN>* glb_temp_working_mem ;
extern AlignedMemoryPool<ALIGN>* glb_temp_lookup_gradient_value_mem;
extern cnn::real* glb_gpu_accessible_host_mem;

template <class Derived>
bool is_valid(const Eigen::MatrixBase<Derived>& x) {
  return ((x - x).array() == (x - x).array()).all();
}

Trainer::~Trainer() {
}

/** 
@scale : proportional to the number of samples trained in parallel 
*/
cnn::real Trainer::clip_gradients(cnn::real samples) {
    cnn::real gscale = 1;

    if (clipping_enabled) {
        if (clipping_type == simple_clipping)
            model->simple_gradient_clipping(clip_threshold * samples);
        else{
            cnn::real gg = model->gradient_l2_norm();
            if (gg > clip_threshold * samples) {
                ++clips;
                gscale = (clip_threshold * samples) / gg;
            }
        }
    }
    return gscale;
}

cnn::real Trainer::clip_gradients(cnn::real samples, cnn::real pre_compued_grd_norm) {
    cnn::real gscale = 1;
    if (clipping_enabled) {
        cnn::real gg = pre_compued_grd_norm;
        if (gg > clip_threshold * samples) {
            ++clips;
            gscale = (clip_threshold * samples) / gg;
        }
    }
    return gscale;
}

void SimpleSGDTrainer::update(cnn::real nutt, cnn::real scale) {
    update(model->lookup_parameters_list(), model->parameters_list(), nutt, scale);
}

void SimpleSGDTrainer::update(const std::vector<LookupParameters*> &lookup_params, const std::vector<Parameters*> &params, cnn::real samples, cnn::real scale) {
  const cnn::real gscale = clip_gradients(samples);
  cnn::real nutt_scale = 1.0 / samples;

  for (auto p : params) {
#if HAVE_CUDA
    gpu::sgd_update(p->values.d.size(), p->g.v, p->values.v, eta * scale * gscale * nutt_scale, lambda);
#else
    auto reg = (*p->values) * lambda;
    *p->values -= nutt_scale * (eta * scale * gscale) * *p->g + reg;
#endif
    p->clear();
  }

#ifdef HAVE_CUDA
  vector<cudaStream_t> streams;
  for (auto p : lookup_params) {
      for (auto g : p->grads) {
          cudaStream_t cs;
          CUDA_CHECK(cudaStreamCreate(&cs));
          streams.push_back(cs);
      }
  }
#endif

  unsigned k = 0;
  for (auto p : lookup_params) {
    for (auto g : p->grads) {
      unsigned i = g.first;
#if HAVE_CUDA
#ifdef USE_CPU_FOR_LOOKUP_PARAM
      gpu::sgd_update(p->values_for_non_zero_grads[i].d.size(), p->grads[i].v, p->values_for_non_zero_grads[i].v, eta * scale * gscale * nutt_scale, lambda);
      CUDA_CHECK(cudaMemcpyAsync(p->values[i].v, p->values_for_non_zero_grads[i].v, p->values[i].d.size() * sizeof(cnn::real), cudaMemcpyDeviceToHost, streams[k++]));
#else
      gpu::sgd_update(p->values[i].d.size(), p->grads[i].v, p->values[i].v, eta * scale * gscale * nutt_scale, lambda);
#endif
#else
      auto reg = (*p->values[i]) * lambda;
      *p->values[i] -= *p->grads[i] * (eta * scale * gscale * nutt_scale) + reg;
#endif
    }
    p->clear();
  }

#ifdef HAVE_CUDA
  CUDA_CHECK(cudaDeviceSynchronize());

  for (k = 0; k < streams.size(); k++)
      CUDA_CHECK(cudaStreamDestroy(streams[k]));
#endif

  ++updates;
}

void MomentumSGDTrainer::update(cnn::real nutt, cnn::real scale) {
  // executed on the first iteration to create vectors to
  // store the velocity
  if (!velocity_allocated) {
    vp = AllocateShadowParameters(*model);
    vlp = AllocateShadowLookupParameters(*model);
    velocity_allocated = true;
  }

  const cnn::real gscale = clip_gradients(nutt);
  cnn::real nutt_scale = 1.0 / nutt;
  unsigned pi = 0;
  for (auto p : model->parameters_list()) {
    Tensor& v = vp[pi++].h;
#if HAVE_CUDA
    gpu::sgd_momentum_update(p->values.d.size(), p->g.v, p->values.v, v.v, eta * scale * gscale * nutt_scale, lambda, momentum);
#else
    auto reg = *p->values * lambda;
    (*v) = momentum * (*v) - (eta * scale * gscale*nutt_scale) * (*p->g);
//    if (verbose)
//    {
//        cout << "name= " << p->name << " v= " << p->values.v[0] << " g= " << p->g.v[0] << " dv=" << v.v[0] << endl;
//    }
    *p->values += *v - reg;
#endif
    p->clear();
  }
  pi = 0;
  for (auto p : model->lookup_parameters_list()) {
    vector<Tensor>& vx = vlp[pi++].h;
    for (auto g : p->grads) {
        unsigned i = g.first;
#if HAVE_CUDA
#ifdef USE_CPU_FOR_LOOKUP_PARAM
        gpu::sgd_momentum_update(p->values_for_non_zero_grads[i].d.size(), p->grads[i].v, p->values_for_non_zero_grads[i].v, vx[i].v, eta * scale * gscale * nutt_scale, lambda, momentum);
        CUDA_CHECK(cudaMemcpy(p->values[i].v, p->values_for_non_zero_grads[i].v, p->values[i].d.size() * sizeof(cnn::real), cudaMemcpyDeviceToHost));
#else
        gpu::sgd_momentum_update(p->values[i].d.size(), p->grads[i].v, p->values[i].v, vx[i].v, eta * scale * gscale * nutt_scale, lambda, momentum);
#endif
#else
      Tensor& v = vx[i];
      auto reg = (*p->values[i]) * lambda;
      (*v) = momentum * (*v) - (eta * scale * gscale*nutt_scale) * (*p->grads[i]);
      *p->values[i] += *v - reg;
#endif
    }
    p->clear();
  }
  ++updates;
}

void AdagradTrainer::update(cnn::real nsamples, cnn::real scale) {
  unsigned pi;
  if (!shadow_params_allocated) {
    vp = AllocateShadowParameters(*model);
    vlp = AllocateShadowLookupParameters(*model);
    shadow_params_allocated = true;
  }

  pi = 0;
  const cnn::real gscale = clip_gradients(nsamples);
  for (auto p : model->parameters_list()) {
    Tensor& v = vp[pi++].h;
    auto reg = (*p->values) * lambda;
    auto g2 = (*p->g).cwiseProduct(*p->g);
    (*v) += g2;
    auto delta = -(eta * scale * gscale) * (*p->g).cwiseQuotient(((*v).array() + epsilon).matrix().cwiseSqrt());
    *p->values += delta - reg;
    p->clear();
  }

  pi = 0;
  for (auto p : model->lookup_parameters_list()) {
    vector<Tensor>& vx = vlp[pi++].h;
    for (auto g : p->grads) {
      unsigned i = g.first;
      Tensor& v = vx[i];
      auto reg = (*p->values[i]) * lambda;
      auto g2 = (*p->grads[i]).cwiseProduct(*p->grads[i]);
      (*v) += g2;
      auto delta = -(eta * scale * gscale) * (*p->grads[i]).cwiseQuotient(((*v).array() + epsilon).matrix().cwiseSqrt());
      *p->values[i] += delta - reg;
    }
    p->clear();
  }

  ++updates;
}

void AdadeltaTrainer::update(cnn::real nutt, cnn::real scale) {
  unsigned pi;
  if (!shadow_params_allocated) {
    hg = AllocateShadowParameters(*model);
    hlg = AllocateShadowLookupParameters(*model);
    hd = AllocateShadowParameters(*model);
    hld = AllocateShadowLookupParameters(*model);

    /*pi = 0;
    for (auto p : model->parameters_list()) {
      TensorTools::Constant(hg[pi].h, epsilon);
      TensorTools::Constant(hd[pi].h, epsilon);
      ++pi;
    }

    pi = 0;
    for (auto p : model->lookup_parameters_list()) {
      vector<Tensor>& hgx = hlg[pi].h;
      vector<Tensor>& hdx = hld[pi].h;
      for (unsigned i = 0; i < hgx.size(); ++i) {
        TensorTools::Constant(hgx[i], epsilon);
        TensorTools::Constant(hdx[i], epsilon);
      }
      ++pi;
    }*/

    shadow_params_allocated = true;
  }

  const cnn::real gscale = clip_gradients(nutt);
  cnn::real nutt_scale = 1.0 / nutt;
  pi = 0;
  for (auto p : model->parameters_list()) {
    auto& g = (scale * gscale * nutt_scale) * *p->g;
    Tensor& hgv = hg[pi].h;
    Tensor& hdv = hd[pi].h;
    auto reg = (*p->values) * lambda;
    auto g2 = g.cwiseProduct(g);
    *hgv = rho * *hgv + (1.0 - rho) * g2;
    auto num = -g.cwiseProduct(((*hdv).array() + epsilon).matrix().cwiseSqrt());
    auto den = ((*hgv).array() + epsilon).matrix().cwiseSqrt();
    auto delta = num.cwiseQuotient(den);
    auto d2 = delta.cwiseProduct(delta);
    *hdv = rho * *hdv + (1.0 - rho) * d2;
    *p->values += delta - reg;
    p->clear();
    pi++;
  }

  pi = 0;
  for (auto p : model->lookup_parameters_list()) {
    vector<Tensor>& hgvx = hlg[pi].h;
    vector<Tensor>& hdvx = hld[pi].h;
    for (auto q : p->grads) {
      unsigned i = q.first;
      Tensor& hgv = hgvx[i];
      Tensor& hdv = hdvx[i];
      auto& g = scale * gscale * nutt_scale * *p->grads[i];
      auto reg = (*p->values[i]) * lambda;
      auto g2 = g.cwiseProduct(g);
      *hgv = rho * *hgv + (1.0 - rho) * g2;
      auto num = -g.cwiseProduct(((*hdv).array() + epsilon).matrix().cwiseSqrt());
      auto den = ((*hgv).array() + epsilon).matrix().cwiseSqrt();
      auto delta = num.cwiseQuotient(den);
      auto d2 = delta.cwiseProduct(delta);
      *hdv = rho * *hdv + (1.0 - rho) * d2;
      *p->values[i] += delta - reg;
    }
    p->clear();
    pi++;
  }
  ++updates;
}

void RmsPropTrainer::compute_gradient_norm(
    std::vector<Parameters*> plist, std::vector<cnn::real>& vpgrd_norm,
    std::vector<LookupParameters*> llist, std::vector<cnn::real>& vl_grd_norm)
{
    cnn::real* v_norm;

    /// get the number of parameters for parm and lookup_param
    vector<int> i_mdl_size(2, plist.size());
    i_mdl_size[1] = 0;
    for (auto p : llist) {
        for (auto i : p->grads) i_mdl_size[1]++;
    }

    int i_mdl_total_size = i_mdl_size[0] + i_mdl_size[1];
#ifdef HAVE_CUDA
    v_norm = (cnn::real*) glb_temp_working_mem->allocate(sizeof(cnn::real) * i_mdl_total_size);
#else
    v_norm = (cnn::real*)malloc(sizeof(cnn::real) * i_mdl_total_size);
    if (v_norm == nullptr)
    {
        cerr << "cannot allocate space" << endl;
        runtime_error("cannot allocate space");
    }
#endif

    int pi = 0;
    for (auto p : plist) {
#if HAVE_CUDA
        gpu::l2_norm_reducer(p->g.d.size(), p->g.v, v_norm + pi, true, false);
#else
        cnn::real g2 = (*p->g).squaredNorm();
        *(v_norm + pi) = g2;
#endif
        pi++;
    }

    for (auto p : llist) {
        for (auto g : p->grads) {
            unsigned i = g.first;
#if HAVE_CUDA
            gpu::l2_norm_reducer(p->grads[i].d.size(), p->grads[i].v, v_norm + pi, true, false);
#else
            cnn::real g2 = (*p->grads[i]).squaredNorm();
            *(v_norm + pi) = g2;
#endif
            pi++;
        }
    }

    if (i_mdl_total_size > GPU_ALLOC_HOST_MEM_SIZE)
    {
        cerr << "out of memory for p_host_memory" << endl;
        throw("out of memory for p_host_memory");
    }

    vpgrd_norm.resize(i_mdl_size[0]);
#ifdef HAVE_CUDA
    CUDA_CHECK(cudaMemcpy(glb_gpu_accessible_host_mem, v_norm, sizeof(cnn::real)*(i_mdl_size[0] + i_mdl_size[1]), cudaMemcpyDeviceToHost));
#pragma parallel for
    for (int i = 0; i < i_mdl_size[0]; i++)
        vpgrd_norm[i] = *(glb_gpu_accessible_host_mem + i);

    vl_grd_norm.resize(i_mdl_size[1]);
#pragma parallel for
	for (int i = 0; i < i_mdl_size[1]; i++)
        vl_grd_norm[i] = *(glb_gpu_accessible_host_mem + i + i_mdl_size[0]);
#else
#pragma parallel for
    for (int i = 0; i < i_mdl_size[0]; i++)
        vpgrd_norm[i] = *(v_norm + i);

    vl_grd_norm.resize(i_mdl_size[1]);
#pragma parallel for
    for (int i = 0; i < i_mdl_size[1]; i++)
        vl_grd_norm[i] = *(v_norm + i + i_mdl_size[0]);
#endif

#ifdef HAVE_CUDA
    glb_temp_working_mem->dealocate(sizeof(cnn::real) * i_mdl_total_size);
#else
    free(v_norm);
#endif
}

void RmsPropTrainer::update(cnn::real nutt, cnn::real scale) 
{
  unsigned pi = 0;
  if (!shadow_params_allocated) {
    hg.resize(model->parameters_list().size());

    pi = 0;
    hlg.resize(model->lookup_parameters_list().size());
    for (auto p : model->lookup_parameters_list()) {
      hlg[pi++].resize(p->size());
    }

    shadow_params_allocated = true;
  }

  /// compute norm of gradients
  vector<cnn::real> vpgrd_norm;
  vector<cnn::real> vlgrd_norm;
  compute_gradient_norm(model->parameters_list(), vpgrd_norm, model->lookup_parameters_list(), vlgrd_norm);

  cnn::real gg = 0;
  for (auto & p : vpgrd_norm)
      gg += p;
  for (auto & p : vlgrd_norm)
      gg += p;
  gg = sqrt(gg);

  const cnn::real gscale = clip_gradients(nutt, gg);

  pi = 0;
  for (auto p : model->parameters_list()) {
    cnn::real& d2 = hg[pi];
#if HAVE_CUDA
    gpu::rmsprop_update(p->values.d.size(), p->g.v, p->values.v, &d2, eta * scale * gscale, lambda, rho, epsilon, vpgrd_norm[pi]);
#else
    auto reg = (*p->values) * lambda;
    cnn::real g2 = (*p->g).squaredNorm();
    d2 = rho * d2 + (1.0 - rho) * g2;
    *p->values -= ((eta * scale * gscale / sqrt(d2 + epsilon)) * *p->g + reg);
#endif
    pi++;
    p->clear();
  }

  pi = 0;
  int li = 0;
  for (auto p : model->lookup_parameters_list()) {
    vector<cnn::real>& hlgx = hlg[pi];
    for (auto g : p->grads) {
        unsigned i = g.first;
        cnn::real& d2 = hlgx[i];
#if HAVE_CUDA
#ifdef USE_CPU_FOR_LOOKUP_PARAM
        gpu::rmsprop_update(p->values_for_non_zero_grads[i].d.size(), p->grads[i].v, p->values_for_non_zero_grads[i].v, &d2, eta * scale * gscale, lambda, rho, epsilon, vlgrd_norm[li]);
        CUDA_CHECK(cudaMemcpy(p->values[i].v, p->values_for_non_zero_grads[i].v, p->values[i].d.size() * sizeof(cnn::real), cudaMemcpyDeviceToHost));
#else
        gpu::rmsprop_update(p->values[i].d.size(), p->grads[i].v, p->values[i].v, &d2, eta * scale * gscale, lambda, rho, epsilon, vlgrd_norm[li]);
#endif
#else
        auto reg = (*p->values[i]) * lambda;
        cnn::real g2 = (*p->grads[i]).squaredNorm();
        d2 = rho * d2 + (1.0 - rho) * g2;
        *p->values[i] -= ((eta * scale * gscale  / sqrt(d2 + epsilon)) * *p->grads[i] + reg);
#endif
      li++;
    }
    p->clear();
    pi++;
  }
  ++updates;
}

void RmsPropWithMomentumTrainer::update(cnn::real nutt, cnn::real scale) {
    unsigned pi = 0;

    if (!shadow_params_allocated) {
        hg.resize(model->parameters_list().size());

        pi = 0;
        hlg.resize(model->lookup_parameters_list().size());
        for (auto p : model->lookup_parameters_list()) {
            hlg[pi++].resize(p->size());
        }

        vp = AllocateShadowParameters(*model);
        vlp = AllocateShadowLookupParameters(*model);

        shadow_params_allocated = true;
    }

    /// compute norm of gradients
    vector<cnn::real> vpgrd_norm;
    vector<cnn::real> vlgrd_norm;
    compute_gradient_norm(model->parameters_list(), vpgrd_norm, model->lookup_parameters_list(), vlgrd_norm);

    cnn::real gg = 0;
    for (auto & p : vpgrd_norm)
        gg += p;
    for (auto & p : vlgrd_norm)
        gg += p;
    gg = sqrt(gg);

    const cnn::real gscale = clip_gradients(nutt, gg);

    /// no need to normalize with the number of sentences as the SGD update
    /// since the gradient norm after sqrt is proportional to the number of sentences. 
    /// during the following rmsprop_momentum_update, g.v will be normalized with the denominator of the gradient norm
    pi = 0;

    for (auto p : model->parameters_list()) {
        cnn::real& d2 = hg[pi];
        Tensor& v = vp[pi].h;
#if HAVE_CUDA
        gpu::rmsprop_momentum_update(p->values.d.size(), p->g.v, p->values.v, v.v, &d2, eta * scale * gscale, lambda, momentum, rho, epsilon, vpgrd_norm[pi]);
#else
        auto reg = (*p->values) * lambda;
        cnn::real g2 = vpgrd_norm[pi];
        d2 = rho * d2 + (1.0 - rho) * g2;

        (*v) = momentum * (*v) - (eta * scale * gscale / sqrt(d2 + epsilon)) * *p->g;

        *p->values += *v - reg; 
#endif
        pi++;
        p->clear();
    }

    pi = 0;
    int li = 0; 
    for (auto p : model->lookup_parameters_list()) {
        vector<cnn::real>& hlgx = hlg[pi];
        vector<Tensor>& vx = vlp[pi++].h;
        for (auto g : p->grads) {
            unsigned i = g.first;
            Tensor& v = vx[i];
            cnn::real& d2 = hlgx[i];
#if HAVE_CUDA
#ifdef USE_CPU_FOR_LOOKUP_PARAM
			cnn::real* gpu_v = (cnn::real*) glb_temp_lookup_gradient_value_mem->allocate(v.d.size() * sizeof(cnn::real));
            CUDA_CHECK(cudaMemcpy(gpu_v, v.v, sizeof(cnn::real)*v.d.size(), cudaMemcpyHostToDevice));
            gpu::rmsprop_momentum_update(p->values_for_non_zero_grads[i].d.size(), p->grads[i].v, p->values_for_non_zero_grads[i].v, gpu_v, &d2, eta * scale * gscale, lambda, momentum, rho, epsilon, vlgrd_norm[li]);
            CUDA_CHECK(cudaMemcpy(p->values[i].v, p->values_for_non_zero_grads[i].v, p->values[i].d.size() * sizeof(cnn::real), cudaMemcpyDeviceToHost));
            CUDA_CHECK(cudaMemcpy(v.v, gpu_v, sizeof(cnn::real)*v.d.size(), cudaMemcpyDeviceToHost));
			glb_temp_lookup_gradient_value_mem->dealocate(v.d.size() * sizeof(cnn::real));
#else
            gpu::rmsprop_momentum_update(p->values[i].d.size(), p->grads[i].v, p->values[i].v, v.v, &d2, eta * scale * gscale, lambda, momentum, rho, epsilon, vlgrd_norm[li]);
#endif
#else
            auto reg = (*p->values[i]) * lambda;
            cnn::real g2 = vlgrd_norm[li];
            d2 = rho * d2 + (1.0 - rho) * g2;
            (*v) = momentum * (*v) - (eta * scale * gscale  / sqrt(d2 + epsilon)) * *p->grads[i];
            *p->values[i] += *v - reg; 
#endif
            li++;
        }
        p->clear();
    }

    ++updates;
}

/// user needs to clear ptr_gnorm and ptr_gnorm_lookup after use
void RmsPropWithMomentumTrainerGPU::compute_gradient_norm(
    std::vector<Parameters*> plist, cnn::real *ptr_gnorm, int size_ptr_gnorm,
    std::vector<LookupParameters*> llist, cnn::real *ptr_gnorm_lookup, int size_ptr_gnorm_lookup)
{
    int pi = 0;
    for (auto p : plist) {
#if HAVE_CUDA
        gpu::l2_norm_reducer(p->g.d.size(), p->g.v, &ptr_gnorm[pi], true, false);
#else
        cnn::real g2 = (*p->g).squaredNorm();
        ptr_gnorm[pi] = g2;
#endif
        pi++;
    }

    pi = 0;
    for (auto p : llist) {
        for (auto g : p->grads) {
            unsigned i = g.first;
#ifdef HAVE_CUDA
            gpu::l2_norm_reducer(p->grads[i].d.size(), p->grads[i].v, &ptr_gnorm_lookup[pi], true, false);
#else
            cnn::real g2 = (*p->grads[i]).squaredNorm();
            ptr_gnorm_lookup[pi] = g2;
#endif
            pi++;
        }
    }
}

void RmsPropWithMomentumTrainerGPU::update(cnn::real nutt, cnn::real scale) {
    unsigned pi = 0;

    if (!shadow_params_allocated) {
        hg = (cnn::real*) cnn_mm_malloc(model->parameters_list().size() * sizeof(cnn::real), CNN_ALIGN);
#ifdef HAVE_CUDA
        CUDA_CHECK(cudaMemset(hg, 0, model->parameters_list().size() * sizeof(cnn::real)));
#else
        memset(hg, 0, model->parameters_list().size()*sizeof(cnn::real));
#endif

        pi = 0;
        hlg.resize(model->lookup_parameters_list().size());
        for (auto p : model->lookup_parameters_list()) {
            hlg[pi] = (cnn::real*) cnn_mm_malloc(p->size() * sizeof(cnn::real), CNN_ALIGN);
#ifdef HAVE_CUDA
            CUDA_CHECK(cudaMemset(hlg[pi], 0, p->size() * sizeof(cnn::real)));
#else
            memset(hlg[pi], 0, p->size() * sizeof(cnn::real));
#endif
            pi++;
        }

        vp = AllocateShadowParameters(*model);
        vlp = AllocateShadowLookupParameters(*model);

        shadow_params_allocated = true;
    }

    /// compute norm of gradients
    cnn::real * vpgrd_each_norm, *vlgrd_each_norm;
    /// get the number of parameters for parm and lookup_param
    int sz_vpgrd_norm = model->parameters_list().size();
    int sz_vlgrd_norm = 0;
    for (auto p : model->lookup_parameters_list()) {
        for (auto i : p->grads) sz_vlgrd_norm++;
    }

    vpgrd_each_norm = (cnn::real*) glb_temp_working_mem->allocate(sizeof(cnn::real)*sz_vpgrd_norm);
    vlgrd_each_norm = (cnn::real*) glb_temp_working_mem->allocate(sizeof(cnn::real)*sz_vlgrd_norm);

    compute_gradient_norm(model->parameters_list(), vpgrd_each_norm, sz_vpgrd_norm,
        model->lookup_parameters_list(), vlgrd_each_norm, sz_vlgrd_norm);

//    display_value(sz_vpgrd_norm, vpgrd_each_norm, "vpgrd_each_norm=");
//    display_value(sz_vlgrd_norm, vlgrd_each_norm, "vlgrd_each_norm=");

#ifdef HAVE_CUDA
    cnn::real * gscale = (cnn::real*) glb_temp_working_mem->allocate(sizeof(cnn::real));;

    gpu::clip_gradients(sz_vpgrd_norm, vpgrd_each_norm,
        sz_vlgrd_norm, vlgrd_each_norm,
        clip_threshold, nutt,
        gscale);
//    display_value(1, gscale, "gscale=");

#else
    cnn::real gg = 0;
    for (int kk = 0; kk < sz_vpgrd_norm; kk++)
        gg += vpgrd_each_norm[kk];
    for (int kk = 0; kk < sz_vlgrd_norm; kk++)
        gg += vlgrd_each_norm[kk];
    gg = sqrt(gg);

    const cnn::real gscale = clip_gradients(nutt, gg);
#endif

    cnn::real nutt_scale = 1.0 / nutt;
    pi = 0;

    for (auto p : model->parameters_list()) {
        Tensor& v = vp[pi].h;
#ifdef HAVE_CUDA
        gpu::rmsprop_smoothing_den(1, rho, vpgrd_each_norm + pi, hg + pi);
//        display_value(1, hg+pi, "hg=");

        gpu::rmsprop_momentum_update(p->values.d.size(),
            hg + pi, p->values.v, p->g.v,
            v.v, gscale, lambda, eta * scale, momentum, epsilon);
//        display_value(1, v.v, "v.v=");
//        display_value(1, p->values.v, "p->values.v=");
#else
        auto reg = (*p->values) * lambda;
        cnn::real g2 = vpgrd_each_norm[pi];
        hg[pi] = rho * hg[pi] + (1.0 - rho) * g2;

        (*v) = momentum * (*v) - (eta * scale * gscale / sqrt(hg[pi] + epsilon)) * *p->g;

        *p->values += *v - reg;
#endif

        pi++;
        p->clear();
    }

    pi = 0;
    int li = 0;
    for (auto p : model->lookup_parameters_list()) {
        vector<Tensor>& vx = vlp[pi].h;
        cnn::real * hlgx = hlg[pi];
        for (auto g : p->grads) {
            unsigned i = g.first;
            Tensor& v = vx[i];
            cnn::real* d2 = hlgx + i;
#if HAVE_CUDA
            gpu::rmsprop_smoothing_den(1, rho, vlgrd_each_norm + li, d2);
//            display_value(1, d2, "hg=");

#ifdef USE_CPU_FOR_LOOKUP_PARAM
            gpu::rmsprop_momentum_update(p->values_for_non_zero_grads[i].d.size(),
                d2, p->values_for_non_zero_grads[i].v, p->grads[i].v, v.v,
                gscale, lambda, eta * scale, momentum, epsilon);
//            display_value(1, v.v, "v.v=");
//            display_value(1, p->values_for_grads[i].v, "p->values_for_grads.v=");
            CUDA_CHECK(cudaMemcpy(p->values[i].v, p->values_for_non_zero_grads[i].v, p->values[i].d.size() * sizeof(cnn::real), cudaMemcpyDeviceToHost));
#else
            gpu::rmsprop_momentum_update(p->values[i].d.size(),
                d2, p->values[i].v, p->grads[i].v, v.v,
                gscale, lambda, eta * scale, momentum, epsilon);
#endif
#else
            auto reg = (*p->values[i]) * lambda;
            cnn::real g2 = vlgrd_each_norm[li];
            *d2 = rho * (*d2) + (1.0 - rho) * g2;
            (*v) = momentum * (*v) - (eta * scale * gscale / sqrt(*d2 + epsilon)) * *p->grads[i];
            *p->values[i] += *v - reg;
#endif

            li++;
        }
        pi++;
        p->clear();
    }

    ++updates;

    glb_temp_working_mem->dealocate(sizeof(cnn::real));;
    glb_temp_working_mem->dealocate(sizeof(cnn::real)*sz_vpgrd_norm);
    glb_temp_working_mem->dealocate(sizeof(cnn::real)*sz_vlgrd_norm);
}

void AdamTrainer::update(cnn::real nutt, cnn::real scale) {
  unsigned pi;
  if (!shadow_params_allocated) {
    m = AllocateShadowParameters(*model);
    lm = AllocateShadowLookupParameters(*model);
    v = AllocateShadowParameters(*model);
    lv = AllocateShadowLookupParameters(*model);
    shadow_params_allocated = true;
  }

  const cnn::real gscale = clip_gradients();
  cnn::real nutt_scale = 1.0 / nutt;
  pi = 0;
  static unsigned t = 0;
  for (auto p : model->parameters_list()) {
    ++t;
    auto g_t = (scale * gscale * nutt_scale) * *p->g;
    auto m_t = *m[pi].h;
    auto v_t = *v[pi].h;
    auto reg = (*p->values) * lambda;
    m_t = beta_1 * m_t + (1 - beta_1) * g_t;
    auto g2 = g_t.cwiseProduct(g_t);
    v_t = beta_2 * v_t + (1 - beta_2) * g2;
    cnn::real s1 = 1 - pow(beta_1, t);
    cnn::real s2 = 1 - pow(beta_2, t);
    auto mhat = m_t / s1;
    auto vhat = v_t / s2;
    auto delta = (-eta * mhat).cwiseQuotient((vhat.array().sqrt() + eps).matrix());
    *p->values += delta - reg;
    p->clear();
    pi++;
  }

  pi = 0;
  for (auto p : model->lookup_parameters_list()) {
    vector<Tensor>& vm = lm[pi].h;
    vector<Tensor>& vv = lv[pi].h;
    for (auto g : p->grads) {
      unsigned i = g.first;
      auto m_t = *vm[i];
      auto v_t = *vv[i];
      auto g_t = scale * gscale * nutt_scale * *p->grads[i];
      auto g2 = g_t.cwiseProduct(g_t);
      auto reg = (*p->values[i]) * lambda;
      m_t = beta_1 * m_t + (1 - beta_1) * g_t;
      v_t = beta_2 * v_t + (1 - beta_2) * g2;
      cnn::real s1 = 1 - pow(beta_1, t);
      cnn::real s2 = 1 - pow(beta_2, t);
      auto mhat = m_t / s1;
      auto vhat = v_t / s2;
      auto delta = (-eta * mhat).cwiseQuotient((vhat.array().sqrt() + eps).matrix());
      *p->values[i] += delta - reg;
    }
    p->clear();
    pi++;
  }
  ++updates;
}

} // namespace cnn
