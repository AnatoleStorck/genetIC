#ifndef _IC_HPP_INCLUDED
#define _IC_HPP_INCLUDED

#include <string>
#include <tuple>
#include <cassert>
#include <functional>
#include <algorithm>
#include <memory>
#include <limits>
#include <iostream>
#include <list>

#include "io/numpy.hpp"

#include "src/simulation/constraints/constraintapplicator.hpp"
#include "src/simulation/constraints/multilevelconstraintgenerator.hpp"

#include "src/tools/filesystem.h"

#include "src/simulation/field/randomfieldgenerator.hpp"
#include "src/simulation/field/multilevelfield.hpp"
#include "src/simulation/field/evaluator.hpp"

#include "src/simulation/particles/multilevelgenerator.hpp"
#include "src/simulation/particles/mapper/onelevelmapper.hpp"
#include "src/simulation/particles/mapper/twolevelmapper.hpp"
#include "src/simulation/particles/mapper/gasmapper.hpp"
#include "src/simulation/particles/mapper/graficmapper.hpp"

#include "src/cosmology/camb.hpp"

//TODO: remove ugly macro
#define for_each_level(level) for(size_t level=0; level<multiLevelContext.getNumLevels(); ++level)

using namespace std;

template<typename T>
class DummyICGenerator;


/*!
   \class ICGenerator
   \brief top level object responsible for coordinating the generation of initial conditions, including genetic modifications.

   This class exposes all methods accessible at user level through main.o

*/
template<typename GridDataType>
class ICGenerator {
protected:

  using T = tools::datatypes::strip_complex<GridDataType>;
  using GridPtrType = std::shared_ptr<grids::Grid<T>>;


  friend class DummyICGenerator<GridDataType>;

  cosmology::CosmologicalParameters<T> cosmology;
  multilevelcontext::MultiLevelContextInformation<GridDataType> multiLevelContext;

  fields::OutputField<GridDataType> outputField;
  constraints::ConstraintApplicator<GridDataType> constraintApplicator;
  constraints::MultiLevelConstraintGenerator<GridDataType> constraintGenerator;
  fields::RandomFieldGenerator<GridDataType> randomFieldGenerator;

  cosmology::CAMB<GridDataType> spectrum;

  int supersample, subsample;               // DM supersampling to perform on zoom grid, and subsampling on base grid

  T xOffOutput, yOffOutput, zOffOutput;


  io::OutputFormat outputFormat;
  string outputFolder, outputFilename;

  bool haveInitialisedRandomComponent;
  // track whether the random realisation has yet been made

  bool exactPowerSpectrum;
  // enforce the exact power spectrum, as in Angulo & Pontzen 2016

  bool allowStrayParticles;
  // "stray" particles are high-res particles outside a high-res grid,
  // constructed through interpolation of the surrounding low-res grid. By default these will be disallowed.

  std::vector<size_t> flaggedParticles;
  std::vector<std::vector<size_t>> zoomParticleArray;


  T x0, y0, z0;

  shared_ptr<particle::mapper::ParticleMapper<GridDataType>> pMapper;
  shared_ptr<particle::mapper::ParticleMapper<GridDataType>> pInputMapper;
  shared_ptr<multilevelcontext::MultiLevelContextInformation<GridDataType>> pInputMultiLevelContext;

  shared_ptr<particle::AbstractMultiLevelParticleGenerator<GridDataType>> pParticleGenerator;

  using RefFieldType = std::vector<GridDataType> &;
  using FieldType = std::vector<GridDataType>;

  tools::ClassDispatch<ICGenerator<GridDataType>, void> &interpreter;


public:
  ICGenerator(tools::ClassDispatch<ICGenerator<GridDataType>, void> &interpreter) :
    outputField(multiLevelContext),
    constraintApplicator(&multiLevelContext, &outputField),
    constraintGenerator(multiLevelContext, cosmology),
    randomFieldGenerator(outputField),
    pMapper(new particle::mapper::ParticleMapper<GridDataType>()),
    interpreter(interpreter) {
    pInputMapper = nullptr;
    pInputMultiLevelContext = nullptr;
    cosmology.hubble = 0.701;   // old default
    cosmology.OmegaBaryons0 = -1.0;
    cosmology.ns = 0.96;      // old default
    cosmology.TCMB = 2.725;
    haveInitialisedRandomComponent = false;
    supersample = 1;
    subsample = 1;
    xOffOutput = 0;
    yOffOutput = 0;
    zOffOutput = 0;
    exactPowerSpectrum = false;
    allowStrayParticles=false;
    pParticleGenerator = std::make_shared<particle::NullMultiLevelParticleGenerator<GridDataType>>();
  }

  ~ICGenerator() {

  }

  void setOmegaM0(T in) {
    cosmology.OmegaM0 = in;
  }

  void setTCMB(T in) {
    cosmology.TCMB = in;
  }

  void setOmegaB0(T in) {
    cosmology.OmegaBaryons0 = in;
    // now that we have gas, mapper may have changed:
    updateParticleMapper();
  }

  void setOmegaLambda0(T in) {
    cosmology.OmegaLambda0 = in;
  }

  void setHubble(T in) {
    cosmology.hubble = in;
  }

  void setStraysOn() {
    allowStrayParticles = true;
  }

  void offsetOutput(T x, T y, T z) {
    xOffOutput = x;
    yOffOutput = y;
    zOffOutput = z;
    updateParticleMapper();
  }

  void setSigma8(T in) {
    cosmology.sigma8 = in;
  }

  void setSupersample(int in) {
    supersample = in;
    updateParticleMapper();
  }

  void setSubsample(int in) {
    subsample = in;
    updateParticleMapper();
  }

  void setZ0(T in) {
    cosmology.redshift = in;
    cosmology.scalefactor = 1. / (cosmology.redshift + 1.);
  }


  virtual void initBaseGrid(T boxSize, size_t n) {
    assert(boxSize > 0);

    if (multiLevelContext.getNumLevels() > 0)
      throw std::runtime_error("Cannot re-initialize the base grid");

    if (haveInitialisedRandomComponent)
      throw (std::runtime_error("Trying to initialize a grid after the random field was already drawn"));

    addLevelToContext(spectrum, boxSize, n);
    updateParticleMapper();

  }

  void setns(T in) {
    cosmology.ns = in;
  }

  void initZoomGrid(size_t zoomfac, size_t n) {
    if (haveInitialisedRandomComponent)
      throw (std::runtime_error("Trying to initialize a grid after the random field was already drawn"));

    if (multiLevelContext.getNumLevels() < 1)
      throw std::runtime_error("Cannot initialise a zoom grid before initialising the base grid");

    grids::Grid<T> &gridAbove = multiLevelContext.getGridForLevel(multiLevelContext.getNumLevels() - 1);
    int nAbove = (int) gridAbove.size;

    storeCurrentCellFlagsAsZoomMask(multiLevelContext.getNumLevels());
    vector<size_t> &newLevelZoomParticleArray = zoomParticleArray.back();

    // find boundaries
    int x0, x1, y0, y1, z0, z1;
    int x, y, z;

    x0 = y0 = z0 = (int) gridAbove.size;
    x1 = y1 = z1 = 0;

    // TO DO: wrap the box sensibly

    for (size_t i = 0; i < newLevelZoomParticleArray.size(); i++) {
      std::tie(x, y, z) = gridAbove.getCellCoordinate(newLevelZoomParticleArray[i]);
      if (x < x0) x0 = x;
      if (y < y0) y0 = y;
      if (z < z0) z0 = z;
      if (x > x1) x1 = x;
      if (y > y1) y1 = y;
      if (z > z1) z1 = z;
    }

    // Now see if the zoom the user chose is OK
    int n_user = nAbove / zoomfac;
    if (((x1 - x0) > n_user || (y1 - y0) > n_user || (z1 - z0) > n_user) && !allowStrayParticles) {
      throw (std::runtime_error(
        "Zoom particles do not fit in specified sub-box. Decrease zoom, or choose different particles. (NB wrapping not yet implemented)"));
    }
    // At this point we know things fit. All we need to do is choose
    // the correct offset to get the particles near the centre of the
    // zoom box.

    // Here is the bottom left of the box (assuming things actually fit):
    x = (x0 + x1) / 2 - nAbove / (2 * zoomfac);
    y = (y0 + y1) / 2 - nAbove / (2 * zoomfac);
    z = (z0 + z1) / 2 - nAbove / (2 * zoomfac);

    // Box can't go outside the corners of the parent box (as above, wrapping still to be implemented)
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (z < 0) z = 0;
    if ((unsigned) x > nAbove - nAbove / zoomfac) x = nAbove - nAbove / zoomfac;
    if ((unsigned) y > nAbove - nAbove / zoomfac) y = nAbove - nAbove / zoomfac;
    if ((unsigned) z > nAbove - nAbove / zoomfac) z = nAbove - nAbove / zoomfac;

    initZoomGridWithOriginAt(x, y, z, zoomfac, n);

  }

  void storeCurrentCellFlagsAsZoomMask(size_t level) {
    assert(level>0);

    if(zoomParticleArray.size()<level)
      zoomParticleArray.emplace_back();

    assert(zoomParticleArray.size()>=level);

    grids::Grid<T> &gridAbove = multiLevelContext.getGridForLevel(level-1);

    vector<size_t> &levelZoomParticleArray = zoomParticleArray[level-1];
    levelZoomParticleArray.clear();
    gridAbove.getFlaggedCells(levelZoomParticleArray);
  }

  void initZoomGridWithOriginAt(int x0, int y0, int z0, size_t zoomfac, size_t n) {
    grids::Grid<T> &gridAbove = multiLevelContext.getGridForLevel(multiLevelContext.getNumLevels() - 1);
    int nAbove = int(gridAbove.size);

    storeCurrentCellFlagsAsZoomMask(multiLevelContext.getNumLevels());

    vector<size_t> trimmedParticleArray;
    vector<size_t> &newLevelZoomParticleArray = zoomParticleArray.back();

    int nCoarseCellsOfZoomGrid = nAbove / int(zoomfac);
    int x1, y1, z1;
    size_t missed_particle=0;

    x1 = x0+nCoarseCellsOfZoomGrid;
    y1 = y0+nCoarseCellsOfZoomGrid;
    z1 = z0+nCoarseCellsOfZoomGrid;

    // Make list of the particles, excluding those that fall outside the new high-res box. Alternatively,
    // if allowStrayParticles is true, keep even those outside the high-res box but report the number
    // in this category.
    for (size_t i = 0; i < newLevelZoomParticleArray.size(); i++) {
      bool include=true;
      int xp, yp, zp;
      std::tie(xp, yp, zp) = gridAbove.getCellCoordinate(newLevelZoomParticleArray[i]);
      if (xp < x0 || yp < y0 || zp < z0 || xp >= x1 || yp >= y1 || zp >= z1) {
        missed_particle+=1;
        include = false;
      }

      if(include || allowStrayParticles) {
        trimmedParticleArray.push_back(newLevelZoomParticleArray[i]);
      }
    }

    if(missed_particle>0) {
      cerr << "WARNING: the requested zoom particles do not all fit in the requested zoom window" << endl;
      if(allowStrayParticles) {
        cerr << "         of " << newLevelZoomParticleArray.size() << " particles, " << missed_particle << " will be interpolated from LR grid (stray particle mode)" << endl;
      } else {
        cerr << "         of " << newLevelZoomParticleArray.size() << " particles, " << missed_particle << " have been omitted" << endl;
      }

      cerr << "         to make a new zoom flag list of " << trimmedParticleArray.size() << endl;
    }

    zoomParticleArray.pop_back();
    zoomParticleArray.emplace_back(std::move(trimmedParticleArray));

    Coordinate<T> newOffsetLower = gridAbove.offsetLower + Coordinate<T>(x0, y0, z0) * gridAbove.dx;

    this->addLevelToContext(spectrum, gridAbove.boxsize / zoomfac, n, newOffsetLower);

    grids::Grid<T> &newGrid = multiLevelContext.getGridForLevel(multiLevelContext.getNumLevels() - 1);

    cout << "Initialized a zoom region:" << endl;
    cout << "  Subbox length         = " << newGrid.boxsize << " Mpc/h" << endl;
    cout << "  n                     = " << newGrid.size << endl;
    cout << "  dx                    = " << newGrid.dx << endl;
    cout << "  Zoom factor           = " << zoomfac << endl;
    cout << "  Origin in parent grid = " << x0 << ", " << y0 << ", " << z0 << endl;
    cout << "  Low-left corner       = " << newGrid.offsetLower.x << ", " << newGrid.offsetLower.y << ", "
         << newGrid.offsetLower.z << endl;
    cout << "  Num particles         = " << newLevelZoomParticleArray.size() << endl;

    updateParticleMapper();

    cout << "  Total particles = " << pMapper->size() << endl;
  }

  virtual void
  addLevelToContext(const cosmology::CAMB<GridDataType> &spectrum, T size, size_t nside, const Coordinate<T> &offset = {0, 0, 0}) {
    // This forwards to multiLevelContext but is required because it is overriden in DummyICGenerator,
    // which needs to ensure that grids are synchronised between two different contexts
    multiLevelContext.addLevel(spectrum, size, nside, offset);
  }


  void setSeed(int in) {
    randomFieldGenerator.seed(in);
  }

  void setSeedFourier(int in) {
    randomFieldGenerator.seed(in);
    randomFieldGenerator.setDrawInFourierSpace(true);
    randomFieldGenerator.setReverseRandomDrawOrder(false);
  }

  void setSeedFourierReverseOrder(int in) {
    randomFieldGenerator.seed(in);
    randomFieldGenerator.setDrawInFourierSpace(true);
    randomFieldGenerator.setReverseRandomDrawOrder(true);
  }

  void setExactPowerSpectrumEnforcement() {
    exactPowerSpectrum = true;
  }

  void setCambDat(std::string in) {
    spectrum.read(in, cosmology);
  }

  void setOutDir(std::string in) {
    outputFolder = in;
  }

  void setOutName(std::string in) {
    outputFilename = in;
  }

  void setOutputFormat(int in) {
    outputFormat = static_cast<io::OutputFormat>(in);
    updateParticleMapper();
  }

  string getOutputPath() {
    ostringstream fname_stream;
    if (outputFilename.size() == 0) {
      fname_stream << outputFolder << "/IC_" << tools::datatypes::floatinfo<T>::name << "_z" << cosmology.redshift
                   << "_" << multiLevelContext.getGridForLevel(0).size;
    } else {
      fname_stream << outputFolder << "/" << outputFilename;
    }
    return fname_stream.str();
  }

  virtual void zeroLevel(int level) {
    cerr << "*** Warning: your script calls zeroLevel(" << level << "). This is intended for testing purposes only!"
         << endl;

    if (!haveInitialisedRandomComponent)
      initialiseRandomComponent();

    auto &fieldData = outputField.getFieldForLevel(level).getDataVector();
    std::fill(fieldData.begin(), fieldData.end(), 0);
  }


  virtual void applyPowerSpec() {
    if (this->exactPowerSpectrum) {
      outputField.enforceExactPowerSpectrum();
    } else {
      outputField.applyPowerSpectrum();
    }
  }

  template<typename TField>
  void dumpGridData(int level, const TField &data) {
    grids::Grid<T> &levelGrid = multiLevelContext.getGridForLevel(level);

    ostringstream filename;
    filename << outputFolder << "/grid-" << level << ".npy";

    data.dumpGridData(filename.str());

    filename.str("");

    filename << outputFolder << "/grid-info-" << level << ".txt";

    ofstream ifile;
    ifile.open(filename.str());
    cerr << "Writing to " << filename.str() << endl;

    ifile << levelGrid.offsetLower.x << " " << levelGrid.offsetLower.y << " "
          << levelGrid.offsetLower.z << " " << levelGrid.boxsize << endl;
    ifile << "The line above contains information about grid level " << level << endl;
    ifile << "It gives the x-offset, y-offset and z-offset of the low-left corner and also the box length" << endl;
    ifile.close();
  }

  virtual void saveTipsyArray(string fname) {
    io::tipsy::saveFieldTipsyArray(fname, *pMapper, *pParticleGenerator, outputField);
  }

  virtual void dumpGrid(int level = 0) {
    outputField.toReal();
    dumpGridData(level, outputField.getFieldForLevel(level));
  }

  virtual void dumpGridFourier(int level = 0) {
    fields::Field<complex<T>, T> fieldToWrite = tools::numerics::fourier::getComplexFourierField(
      outputField.getFieldForLevel(level));
    dumpGridData(level, fieldToWrite);
  }

  virtual void dumpPS(int level = 0) {
    auto &field = outputField.getFieldForLevel(level);
    field.toFourier();
    cosmology::dumpPowerSpectrum(field,
                                 multiLevelContext.getCovariance(level),
                                 (getOutputPath() + "_" + ((char) (level + '0')) + ".ps").c_str());
  }


  virtual void initialiseParticleGenerator() {
    // in principle this could now be easily extended to slot in higher order PT or other
    // methods of generating the particles from the fields

    using GridLevelGeneratorType = particle::ZeldovichParticleGenerator<GridDataType>;

    pParticleGenerator = std::make_shared<
      particle::MultiLevelParticleGenerator<GridDataType, GridLevelGeneratorType>>(outputField, cosmology);

  }

  void setInputMapper(std::string fname) {
    DummyICGenerator<GridDataType> pseudoICs(this);
    auto dispatch = interpreter.specify_instance(pseudoICs);
    ifstream inf;
    inf.open(fname);


    if (!inf.is_open())
      throw std::runtime_error("Cannot open IC paramfile for relative_to command");
    cerr << "******** Running commands in" << fname << " to work out relationship ***********" << endl;

    tools::ChangeCwdWhileInScope temporary(tools::getDirectoryName(fname));

    dispatch.run_loop(inf);
    cerr << *(pseudoICs.pMapper) << endl;
    cerr << "******** Finished with" << fname << " ***********" << endl;
    pInputMapper = pseudoICs.pMapper;
    pInputMultiLevelContext = std::make_shared<multilevelcontext::MultiLevelContextInformation<GridDataType>>
      (pseudoICs.multiLevelContext);
  }

  /*! Get the grid on which the output is defined for a particular level.
   *
   * This may differ from the grid on which the fields are defined either because there is an offset or
   * there are differences in the resolution between the output and the literal fields.
   *
   */
  std::shared_ptr<grids::Grid<T>> getOutputGrid(int level = 0) {
    auto gridForOutput = multiLevelContext.getGridForLevel(level).shared_from_this();

    if (xOffOutput != 0 || yOffOutput != 0 || zOffOutput != 0) {
      gridForOutput = std::make_shared<grids::OffsetGrid<T>>(gridForOutput,
                                                             xOffOutput, yOffOutput, zOffOutput);
    }
    if(allowStrayParticles && level>0) {
      gridForOutput = std::make_shared<grids::ResolutionMatchingGrid<T>>(gridForOutput,
                                                                         getOutputGrid(level - 1));
    }
    return gridForOutput;
  }

  void updateParticleMapper() {
    // TODO: This routine contains too much format-dependent logic and should be refactored so that the knowledge
    // resides somewhere in the io namespace

    size_t nLevels = multiLevelContext.getNumLevels();

    if (nLevels == 0)
      return;

    if (outputFormat == io::OutputFormat::grafic) {
      // Grafic format just writes out the grids in turn
      pMapper = std::make_shared<particle::mapper::GraficMapper<GridDataType>>(multiLevelContext);
      return;
    }

    // make a basic mapper for the coarsest grid
    pMapper = std::shared_ptr<particle::mapper::ParticleMapper<GridDataType>>(
      new particle::mapper::OneLevelParticleMapper<GridDataType>(
          getOutputGrid(0)
      ));


    if (nLevels >= 2) {

      for (size_t level = 1; level < nLevels; level++) {

        auto pFine = std::shared_ptr<particle::mapper::ParticleMapper<GridDataType>>(
          new particle::mapper::OneLevelParticleMapper<GridDataType>(getOutputGrid(level)));

        pMapper = std::shared_ptr<particle::mapper::ParticleMapper<GridDataType>>(
          new particle::mapper::TwoLevelParticleMapper<GridDataType>(pMapper, pFine, zoomParticleArray[level - 1]));
      }
    }

    if (cosmology.OmegaBaryons0 > 0) {

      // Add gas only to the deepest level. Pass the whole pGrid
      // vector if you want to add gas to every level.
      auto gasMapper = pMapper->addGas(cosmology.OmegaBaryons0 / cosmology.OmegaM0,
                                       {multiLevelContext.getGridForLevel(nLevels - 1).shared_from_this()});

      bool gasFirst = outputFormat == io::OutputFormat::tipsy;

      // graft the gas particles onto the start of the map
      if (gasFirst)
        pMapper = std::make_shared<particle::mapper::AddGasMapper<GridDataType>>(
          gasMapper.first, gasMapper.second, true);
      else
        pMapper = std::make_shared<particle::mapper::AddGasMapper<GridDataType>>(
          gasMapper.second, gasMapper.first, false);

    }

    // potentially resample the lowest-level DM grid. Again, this is theoretically
    // more flexible if you pass in other grid pointers.
    if (supersample > 1)
      pMapper = pMapper->superOrSubSampleDM(supersample,
                                            {multiLevelContext.getGridForLevel(nLevels - 1).shared_from_this()}, true);

    if (subsample > 1)
      pMapper = pMapper->superOrSubSampleDM(subsample, {multiLevelContext.getGridForLevel(0).shared_from_this()},
                                            false);

  }

  void reflag() {

    if (pInputMapper != nullptr) {
      pMapper->unflagAllParticles();
      pInputMapper->flagParticles(flaggedParticles);
      pInputMapper->extendParticleListToUnreferencedGrids(multiLevelContext);
      pMapper->extendParticleListToUnreferencedGrids(*pInputMultiLevelContext);
    } else {
      pMapper->unflagAllParticles();
      pMapper->flagParticles(flaggedParticles);
    }
  }


  virtual void write() {
    using namespace io;

    if (!haveInitialisedRandomComponent)
      initialiseRandomComponent();

    initialiseParticleGenerator();

    cerr << "Write, ndm=" << pMapper->size_dm() << ", ngas=" << pMapper->size_gas() << endl;
    cerr << (*pMapper);

    T boxlen = multiLevelContext.getGridForLevel(0).simsize;

    switch (outputFormat) {
      case OutputFormat::gadget2:
      case OutputFormat::gadget3:
        gadget::save(getOutputPath() + ".gadget", boxlen, *pMapper,
                     *pParticleGenerator,
                     cosmology, static_cast<int>(outputFormat));
        break;
      case OutputFormat::tipsy:
        tipsy::save(getOutputPath() + ".tipsy", boxlen, *pParticleGenerator,
                    pMapper, cosmology);
        break;
      case OutputFormat::grafic:
        grafic::save(getOutputPath() + ".grafic", *pParticleGenerator, multiLevelContext, cosmology);
        break;
      default:
        throw std::runtime_error("Unknown output format");
    }

  }

  void initialiseRandomComponent() {
    if (haveInitialisedRandomComponent)
      throw (std::runtime_error("Trying to re-draw the random field after it was already initialised"));

    randomFieldGenerator.draw();
    applyPowerSpec();

    haveInitialisedRandomComponent = true;
  }


protected:

  int deepestLevelWithParticlesSelected() {
    for (size_t i = multiLevelContext.getNumLevels() - 1; i >= 0; --i) {
      if (multiLevelContext.getGridForLevel(i).hasFlaggedCells())
        return i;
    }
    throw std::runtime_error("No level has any particles selected");
  }

  int deepestLevel() {
    //TODO: can this be removed?
    return multiLevelContext.getNumLevels();
  }

  T get_wrapped_delta(T x0, T x1) {
    return multiLevelContext.getGridForLevel(0).getWrappedDelta(x0, x1);
  }


  void getCentre() {
    x0 = 0;
    y0 = 0;
    z0 = 0;

    int level = deepestLevelWithParticlesSelected();

    std::vector<size_t> particleArray;
    grids::Grid<T> &grid = multiLevelContext.getGridForLevel(level);
    grid.getFlaggedCells(particleArray);

    auto p0_location = grid.getCellCentroid(particleArray[0]);

    for (size_t i = 0; i < particleArray.size(); i++) {
      auto pi_location = grid.getCellCentroid(particleArray[i]);
      x0 += get_wrapped_delta(pi_location.x, p0_location.x);
      y0 += get_wrapped_delta(pi_location.y, p0_location.y);
      z0 += get_wrapped_delta(pi_location.z, p0_location.z);
    }
    x0 /= particleArray.size();
    y0 /= particleArray.size();
    z0 /= particleArray.size();

    cerr << "Centre of region is " << setprecision(12) << x0 << " " << y0 << " " << z0 << endl;
  }


  void appendParticleIdFile(std::string filename) {

    cerr << "Loading " << filename << endl;

    io::getBuffer(flaggedParticles, filename);
    size_t size = flaggedParticles.size();
    std::sort(flaggedParticles.begin(), flaggedParticles.end());
    flaggedParticles.erase(std::unique(flaggedParticles.begin(), flaggedParticles.end()),
                           flaggedParticles.end());
    if (flaggedParticles.size() < size)
      cerr << "  ... erased " << size - flaggedParticles.size() << " duplicate particles" << endl;
    cerr << "  -> total number of particles is " << flaggedParticles.size() << endl;

    reflag();
  }

  void loadParticleIdFile(std::string filename) {
    flaggedParticles.clear();
    appendParticleIdFile(filename);
  }


public:


  void loadID(string fname) {
    loadParticleIdFile(fname);
    getCentre();
  }

  void appendID(string fname) {
    appendParticleIdFile(fname);
    getCentre();
  }

  virtual void dumpID(string fname) {
    std::vector<size_t> results;
    cerr << "dumpID using current mapper:" << endl;
    cerr << (*pMapper);
    pMapper->getFlaggedParticles(results);
    io::dumpBuffer(results, fname);
  }

  void centreParticle(long id) {
    std::tie(x0, y0, z0) = multiLevelContext.getGridForLevel(0).getCellCentroid(id);
  }

  void selectNearest() {
    auto &grid = multiLevelContext.getGridForLevel(deepestLevel() - 1);
    pMapper->unflagAllParticles();
    size_t id = grid.getClosestIdNoWrap(Coordinate<T>(x0, y0, z0));
    cerr << "selectNearest " << x0 << " " << y0 << " " << z0 << " " << id << " " << endl;
    grid.flagCells({id});

  }

  void select(std::function<bool(T, T, T)> inclusionFunction) {
    T delta_x, delta_y, delta_z;
    T xp, yp, zp;

    flaggedParticles.clear();


    // unflag all grids first. This can't be in the loop below in case there are subtle
    // relationships between grids (in particular the ResolutionMatchingGrid which actually
    // points to two levels simultaneously).
    for_each_level(level) {
      getOutputGrid(level)->unflagAllCells();
    }

    for_each_level(level) {
      std::vector<size_t> particleArray;
      auto grid = getOutputGrid(level);
      size_t N = grid->size3;
      for (size_t i = 0; i < N; i++) {
        std::tie(xp, yp, zp) = grid->getCellCentroid(i);
        delta_x = get_wrapped_delta(xp, x0);
        delta_y = get_wrapped_delta(yp, y0);
        delta_z = get_wrapped_delta(zp, z0);
        if (inclusionFunction(delta_x, delta_y, delta_z))
          particleArray.push_back(i);
      }
      grid->flagCells(particleArray);

    }
  }

  void selectSphere(float radius) {
    T r2 = radius * radius;
    select([r2](T delta_x, T delta_y, T delta_z) -> bool {
      T r2_i = delta_x * delta_x + delta_y * delta_y + delta_z * delta_z;
      return r2_i < r2;
    });

  }

  void selectCube(float side) {
    T side_by_2 = side / 2;
    select([side_by_2](T delta_x, T delta_y, T delta_z) -> bool {
      return abs(delta_x) < side_by_2 && abs(delta_y) < side_by_2 && abs(delta_z) < side_by_2;
    });
  }


  void setCentre(T xin, T yin, T zin) {
    x0 = xin;
    y0 = yin;
    z0 = zin;
  }

  auto calcConstraint(string name_in) {
    auto constraint = constraintGenerator.calcConstraintForAllLevels(name_in);
    constraint.toFourier();
    return constraint;
  }

  void calculate(string name) {
    if (!haveInitialisedRandomComponent)
      initialiseRandomComponent();

    auto constraint_field = calcConstraint(name);
    auto val = constraint_field.innerProduct(outputField);

    cout << name << ": calculated value = " << val << endl;
  }

  virtual void constrain(string name, string type, float value) {
    if (!haveInitialisedRandomComponent)
      initialiseRandomComponent();

    bool relative = false;
    if (strcasecmp(type.c_str(), "relative") == 0) {
      relative = true;
    } else if (strcasecmp(type.c_str(), "absolute") != 0) {
      throw runtime_error("Constraint type must be either 'relative' or 'absolute'");
    }

    T constraint = value;
    auto vec = calcConstraint(name);

    T initv = vec.innerProduct(outputField).real();

    if (relative) constraint *= initv;

    cout << name << ": initial value = " << initv << ", constraining to " << constraint << endl;
    constraintApplicator.add_constraint(std::move(vec), constraint, initv);

  }

  void cov() {
    constraintApplicator.print_covariance();
  }


  virtual void fixConstraints() {
    if (!haveInitialisedRandomComponent)
      initialiseRandomComponent();

    constraintApplicator.applyModifications();
  }

  virtual void done() {
    T pre_constraint_chi2 = outputField.getChi2();
    cerr << "BEFORE constraints chi^2=" << pre_constraint_chi2 << endl;
    fixConstraints();
    T post_constraint_chi2 = outputField.getChi2();
    cerr << "AFTER  constraints chi^2=" << post_constraint_chi2 << endl;
    cerr << "             delta-chi^2=" << post_constraint_chi2 - pre_constraint_chi2 << endl;
    write();
  }

  void reverse() {
    for_each_level(level) {
      auto &field = outputField.getFieldForLevel(level);
      size_t N = field.getGrid().size3;
      auto &field_data = field.getDataVector();
      for (size_t i = 0; i < N; i++)
        field_data[i] = -field_data[i];
    }
  }

  void reseedSmallK(T kmax, int seed) {

    T k2max = kmax * kmax;

    // take a copy of all the fields
    std::vector<FieldType> fieldCopies;
    for_each_level(level) {
      auto &field = outputField.getFieldForLevel(level);
      field.toFourier();
      fieldCopies.emplace_back(field);
    }

    // remake the fields with the new seed
    randomFieldGenerator.seed(seed);
    initialiseRandomComponent();

    // copy back the old field
    for_each_level(level) {
      auto &fieldOriginal = fieldCopies[level];
      auto &field = outputField.getFieldForLevel(level);
      auto &grid = field.getGrid();
      field.toFourier();
      T k2;
      size_t N = grid.size3;
      for (size_t i = 0; i < N; i++) {
        k2 = grid.getFourierCellKSquared(i);
        if (k2 > k2max && k2 != 0) {
          field[i] = fieldOriginal[i];
        }
      }
    }

  }

  void reverseSmallK(T kmax) {

    T k2max = kmax * kmax;


    for_each_level(level) {
      T k2_g_min = std::numeric_limits<T>::max();
      T k2_g_max = 0.0;
      size_t modes_reversed = 0;
      auto &field = outputField.getFieldForLevel(level);
      field.toFourier();
      auto &fieldData = field.getDataVector();
      const grids::Grid<T> &grid = field.getGrid();
      size_t tot_modes = grid.size3;
      T k2;
      size_t N = grid.size3;
      for (size_t i = 0; i < N; i++) {
        k2 = grid.getFourierCellKSquared(i);
        if (k2 < k2max && k2 != 0) {
          fieldData[i] = -fieldData[i];
          modes_reversed++;
        }
        if (k2 < k2_g_min && k2 != 0)
          k2_g_min = k2;
        if (k2 > k2_g_max)
          k2_g_max = k2;
      }
      cerr << "reverseSmallK: k reversal at " << sqrt(k2max) << "; grid was in range " << sqrt(k2_g_min) << " to " <<
           sqrt(k2_g_max) << endl;
      cerr << "               modes reversed = " << modes_reversed << " of " << tot_modes << endl;
    }

  }


};

#endif
