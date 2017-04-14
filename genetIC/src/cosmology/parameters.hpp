#ifndef _COSMO_HPP_INCLUDED
#define _COSMO_HPP_INCLUDED

#include <functional>
#include <climits>


namespace cosmology {

  template<typename FloatType>
  struct CosmologicalParameters {
    FloatType OmegaM0, OmegaLambda0, OmegaBaryons0, hubble, redshift;
    FloatType scalefactor, sigma8, ns, TCMB;
  };

  template<typename FloatType>
  FloatType growthFactor(const CosmologicalParameters<FloatType> &cosmology) {
    const FloatType a = cosmology.scalefactor;
    const FloatType Om = cosmology.OmegaM0;
    const FloatType Ol = cosmology.OmegaLambda0;

    FloatType Hsq = cosmology.OmegaM0 / powf(a, 3.0) + (1. - Om - Ol) / a / a + Ol;
    FloatType d = 2.5 * a * Om / powf(a, 3.0) / Hsq / (powf(Om / Hsq / a / a / a, 4. / 7.) - Ol / Hsq +
                                                       (1. + 0.5 * Om / powf(a, 3.0) / Hsq) *
                                                       (1. + 1. / 70. * Ol / Hsq));
    // TODO: check accuracy and/or simplify this expression

    return d;
  }

  template<typename FloatType>
  CosmologicalParameters<FloatType>
  cosmologyAtRedshift(const CosmologicalParameters<FloatType> &referenceCosmology, float redshift) {
    CosmologicalParameters<FloatType> retVal(referenceCosmology);
    retVal.scalefactor = 1. / (1. + redshift);
    return retVal;
  }

  /** Dump an estimated power spectrum for the field, alongside the specified theory power spectrum, to disk
    */
  template<typename DataType, typename FloatType=tools::datatypes::strip_complex<DataType>>
  void dumpPowerSpectrum(const fields::Field<DataType> &field,
                         const std::vector<FloatType> &P0, const std::string &filename) {


    int res = field.getGrid().size;
    int nBins = 100;
    std::vector<FloatType> inBin(nBins);
    std::vector<FloatType> kbin(nBins);
    std::vector<FloatType> Gx(nBins);
    std::vector<FloatType> Px(nBins);

    const FloatType Boxlength = field.getGrid().boxsize;

    FloatType kmax = M_PI / Boxlength * (FloatType) res, kmin = 2.0f * M_PI / (FloatType) Boxlength, dklog =
      log10(kmax / kmin) / nBins, kw = 2.0f * M_PI / (FloatType) Boxlength;

    int ix, iy, iz, idx, idx2;
    FloatType kfft;

    for (ix = 0; ix < nBins; ix++) {
      inBin[ix] = 0;
      Gx[ix] = 0.0f;
      kbin[ix] = 0.0f;
    }


    for (ix = 0; ix < res; ix++)
      for (iy = 0; iy < res; iy++)
        for (iz = 0; iz < res; iz++) {
          idx = (ix * res + iy) * (res) + iz;

          // determine mode modulus

          auto fieldValue = field[idx];
          FloatType vabs = std::abs(fieldValue);
          vabs *= vabs * tools::numerics::fourier::getFourierCellWeight(field, idx);

          int iix, iiy, iiz;

          if (ix > res / 2) iix = ix - res; else iix = ix;
          if (iy > res / 2) iiy = iy - res; else iiy = iy;
          if (iz > res / 2) iiz = iz - res; else iiz = iz;

          kfft = sqrt(iix * iix + iiy * iiy + iiz * iiz);
          FloatType k = kfft * kw;

          /*
          // correct for aliasing, formula from Jing (2005), ApJ 620, 559
          // assume isotropic aliasing (approx. true for k<kmax=knyquist)
          // this formula is for CIC interpolation scheme, which we use <- only needed for Powerspectrum

          FloatType JingCorr = (1.0f - 2.0f / 3.0f * sin(M_PI * k / kmax / 2.0f) * sin(M_PI * k / kmax / 2.0f));
          vabs /= JingCorr;
           */

          //.. logarithmic spacing in k
          idx2 = (int) ((1.0f / dklog * log10(k / kmin)));

          if (k >= kmin && k < kmax) {

            Gx[idx2] += vabs / (FloatType) (res * res * res); //because FFT is now normalised with 1/sqrt(Ntot)
            Px[idx2] += P0[idx];
            kbin[idx2] += k;
            inBin[idx2]++;

          } else { continue; }

        }


    //... convert to physical units ...
    std::ofstream ofs(filename);


    FloatType psnorm = powf(Boxlength / (2.0 * M_PI), 3.0);

    for (ix = 0; ix < nBins; ix++) {

      if (inBin[ix] > 0) {


        ofs << std::setw(16) << pow(10., log10(kmin) + dklog * (ix + 0.5))
            << std::setw(16) << kbin[ix] / inBin[ix]
            << std::setw(16) << (FloatType) (Px[ix] / inBin[ix]) * psnorm
            << std::setw(16) << (FloatType) (Gx[ix] / inBin[ix]) * psnorm
            << std::setw(16) << inBin[ix]
            << std::endl;

      }
    }
    ofs.close();


  }

  /** Convert the density field to a potential field, in-place. */
  template<typename DataType, typename FloatType=tools::datatypes::strip_complex<DataType>>
  void densityToPotential(fields::Field<DataType, FloatType> &field, const CosmologicalParameters<FloatType> &cosmo) {


    field.toFourier();

    const FloatType Boxlength = field.getGrid().boxsize;
    const FloatType a = cosmo.scalefactor;
    const FloatType Om = cosmo.OmegaM0;
    const size_t res = field.getGrid().size;

    long i;
    FloatType prefac =
      3. / 2. * Om / a * 100. * 100. / (3. * 100000.) / (3. * 100000.); // =3/2 Om0/a * (H0/h)^2 (h/Mpc)^2 / c^2 (km/s)
    FloatType kw = 2.0f * M_PI / (FloatType) Boxlength, k_inv;

    size_t k1, k2, k3, kk1, kk2, kk3;

    for (k1 = 0; k1 < res; k1++) {
      for (k2 = 0; k2 < res; k2++) {
        for (k3 = 0; k3 < res; k3++) {
          // TODO: refactor this to use the provided routine for getting k coordinates in Grid class
          i = (k1 * res + k2) * (res) + k3;

          if (k1 > res / 2) kk1 = k1 - res; else kk1 = k1;
          if (k2 > res / 2) kk2 = k2 - res; else kk2 = k2;
          if (k3 > res / 2) kk3 = k3 - res; else kk3 = k3;

          k_inv = 1.0 / ((FloatType) (kk1 * kk1 + kk2 * kk2 + kk3 * kk3) * kw * kw);
          if (i == 0)
            k_inv = 0;

          field[i] *= -prefac * k_inv;
        }
      }
    }


  }

}

#endif
