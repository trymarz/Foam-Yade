// YADE-OpenFOAM coupling module
// (c) 2019  Deepak kunhappan : deepak.kunhappan@3sr-grenoble.fr; deepak.kn1990@gmail.com
#ifdef YADE_MPI

#pragma once

#include <lib/base/Logging.hpp>
#include <lib/serialization/Serializable.hpp>
#include <core/Aabb.hpp>
#include <core/Body.hpp>
#include <core/Dispatching.hpp>
#include <core/GlobalEngine.hpp>
#include <core/InteractionContainer.hpp>
#include <core/Omega.hpp>
#include <core/Scene.hpp>
#include <core/Subdomain.hpp>
#include <pkg/common/Sphere.hpp>
#include <vector>
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpragmas"
#pragma GCC diagnostic ignored "-Wsuggest-override"
#pragma GCC diagnostic ignored "-Wcast-function-type"
#include <mpi.h>
#pragma GCC diagnostic pop

namespace yade { // Cannot have #include directive inside.
class Scene;
class Subdomain;
class Interaction;
class BodyContainer;
class FoamCoupling : public GlobalEngine {
private:
	// some variables for MPI_Send/Recv
	const int  sendTag = 500;
	int        rank, commSize;              // for serial Yade-OpenFOAM
	int        foamCommSize, worldCommSize; //
	MPI_Status status;
	int        szdff, localCommSize, localRank, worldRank;
	const int  TAG_GRID_BBOX  = 1001;
	const int  TAG_PRT_DATA   = 1002;
	const int  TAG_FORCE      = 1005;
	const int  TAG_SEARCH_RES = 1004;
	const int  yadeMaster     = 0;
	const int  TAG_SZ_BUFF    = 1003;
	const int  TAG_FLUID_DT   = 1050;
	const int  TAG_YADE_DT    = 1060;
	const int  TAG_SHARED_ID  = 1080;
	const int  TAG_LAMBDA_MODE = 1100;
	bool       serialYade     = false;
	MPI_Comm   FOAMCOMM;  // the communicator used to spawn the FOAM processes.
	MPI_Comm   INTRACOMM; // the merged communicator to communicate with the FOAM processes.
public:
	// clang-format off
		// void getRank();
		void setNumParticles(int);
		void setIdList(const std::vector<int>& );
		void killMPI();
		void updateProcList();
		void resetProcList();
		void exchangeDeltaT();
		bool exchangeData();
        void castTerminate();
		Real getViscousTimeScale();  // not fully implemented, piece of code left in foam.
		void getParticleForce();
		void verifyParticleDetection();
		bool ifFluidDomain(const Body::id_t& );
		int ifSharedId(const Body::id_t& );
		bool checkSharedDomains(const int& );
		int stride;
		void resetFluidDomains();
		void runCoupling();
		void setHydroForce();
		//- lambdaMode 1 only: multiply each body's lambda_ by the received factor,
		//  once per exchange (setHydroForce() runs every DEM step).
		void applyLambdaFactor();
		void StartFoamSolver();
		void SetFoamSolver(const std::string &solverName, unsigned numProcs);

		void buildLocalIds();
		void exchangeDeltaTParallel();
		void insertBodyId(int);
		bool eraseId(int);
		int getNumBodies();
		std::vector<int> getIdList();
		MPI_Comm *myComm_p;
		bool bodyListModified;
		int findRankSharedIndxMap(const std::map<int, int>& , const int& );

		MPI_Comm selfComm() {if (myComm_p) return *myComm_p; else return MPI_COMM_WORLD;}

		// pass python-generated communicator to the c++ side
		// inspired by https://bitbucket.org/mpi4py/mpi4py/src/master/demo/wrap-boost/helloworld.cxx
		void setMyComm(boost::python::object py_comm) {
			if (import_mpi4py() < 0) return;// must be somewhere to initialize mpi4py in c++, else segfault
			PyObject* py_obj = py_comm.ptr();
			myComm_p = PyMPIComm_Get(py_obj);
			if (myComm_p == NULL) LOG_ERROR("invalid COMM received from Python");
		}
		PyObject* getMyComm() {	return PyMPIComm_New(*myComm_p);}
		virtual void action() override;
		virtual ~FoamCoupling(){};

		std::vector<int> bodyList;  // 'global' all Ids across all procs which are in coupling. Used in serial mode  coupling.
		std::vector<double> hydroForce;
		std::vector<double> particleData;
		std::vector<int>  procList;
		//std::vector<Body::id_t> fluidDomains;
		std::vector<std::pair<Body::id_t, std::vector<Body::id_t> > > sharedIds;
		std::vector<std::pair<int, std::map<int, int> > > sharedIdsMapIndx;
		std::vector<std::pair<int, std::vector<double>> > hForce;
		std::vector<std::pair<int, int> > inCommunicationProc;
		std::vector<Body::id_t> localIds; // 'local', those Ids in the present subdomain  that are in coupling, used in parallel mode.
		


		std::string foamSolverName;
		
		int numFoamProcs;

		//std::vector<int> intrFluidRanks;

		Real foamDeltaT;

		long int  dataExchangeInterval=1;
		bool recvdFoamDeltaT;
		bool isGaussianInterp;
		void getFluidDomainBbox();
		void findIntersections();
		bool ifDomainBodies(const shared_ptr<Body>& );
		void sendBodyData();
		void sendIntersectionToFluidProcs();
		int commSzdff;
		int otherCommSz;
		void buildSharedIdsMap();
		int ifSharedIdMap(const Body::id_t& );
		//bool couplingModeParallel = false;
		bool initDone = false;
		void checkFoamVersion();

    YADE_CLASS_BASE_DOC_ATTRS_INIT_CTOR_PY(FoamCoupling,GlobalEngine, "An engine for coupling Yade with the finite volume fluid solver OpenFOAM in parallel." " \n Requirements : Yade compiled with MPI libs, OpenFOAM-6 (openfoam is not required for compilation)." "Yade is executed under MPI environment with OpenFOAM simultaneously, and using MPI communication  routines data is exchanged between the solvers."
   " \n \n 1. Yade broadcasts the particle data -> position, velocity, ang-velocity, radius to all the foam processes as in :yref:`castParticle <FoamCoupling::castParticle>` \n"
  "2. In each foam process, particle is searched.Yade keeps a vector(cpp) of the rank of the openfoam process containing that particular particle (FoamCoupling::procList), using :yref:`updateProcList <FoamCoupling::updateProcList>`\n"
  "3. In simple lagrangian point force coupling Yade recieves the particle hydrodynamic force and torque from the openfoam process, the sender is identified from  the vector :yref:`FoamCoupling::procList`.\n"
  "In the case of Gaussian interpolation, contribution from every  process is summed using function :yref:`sumHydroForce <FoamCoupling::sumHydroForce>`. \n"
  "4. The interval (substepping) is set automatically (:yref:`FoamCoupling::dataExchangeInterval`) based on dtfoam/dtYade, calculated in function :yref:`exchangeDeltaT<FoamCoupling::exchangeDeltaT>`  ",
    ((int,numParticles,1, , "number of particles in coupling."))
    ((int,lambdaMode,0,Attr::readonly,"How the value OpenFOAM sends in force slot 6 is applied, set by OpenFOAM at startup: 0 = rate, integrated into :yref:`State.lambda_` by :yref:`NewtonIntegrator`; 1 = factor, :yref:`State.lambda_` multiplied by it once per exchange."))
    ((int,foamVersion,-1,,"version of OpenFoam environment"))
    ((string,foamPath,"",,"path to OpenFoam"))
    ((double,particleDensity,1, , "particle Density")) //not needed  as this is set in foam
    ((double,fluidDensity,1, ,"fluidDensity")) //not needed  as this is set in foam
//    ((bool,couplingModeParallel,false, ,"set true if Yade-MPI is being used. "))



    ((std::vector<Body::id_t>,fluidDomains, std::vector<Body::id_t>(),,"list of fluid domain bounding fictitious fluid bodies that has the fluid mesh bounds"))
    ,
    ,
    checkFoamVersion();
    ,
    //.add_property("couplingModeParallel",&FoamCoupling::setCouplingMode,&FoamCoupling::getCouplingMode,"coupling mode : if true, parllel coupling between Yade & YALES2")
//     .def("")
//     .def("")
    .def("setIdList", &FoamCoupling::setIdList,boost::python::arg("bodyIdlist"), "list of body ids in hydroForce coupling. (links to :yref: `FoamCoupling::bodyList` vector, used to build particle data :yref:`FoamCoupling::particleData`. :yref:`FoamCoupling::particleData` contains the particle pos, vel, angvel, radius and this is sent to foam. )")
    // .def("getRank", &FoamCoupling::getRank, "Initiallize MPI communicator for coupling. Should be called at the beginning of the script. :yref: `initMPI <FoamCoupling::initMPI>` Initializes  the MPI environment. " )
    .def("killMPI", &FoamCoupling::killMPI, "Destroy MPI, to be called at the end of the simulation, from :yref:`killMPI<FoamCoupling::killMPI>`")
    .def("setNumParticles",&FoamCoupling::setNumParticles,boost::python::arg("numparticles"),"number of particles in coupling")
    .def("insertBodyId", &FoamCoupling::insertBodyId,boost::python::arg("newId"), "insert a new body id for hydrodynamic force coupling")
    .def("eraseId", &FoamCoupling::eraseId,boost::python::arg("idToErase"), "remove a body from hydrodynamic force coupling")
    .def("getNumBodies", &FoamCoupling::getNumBodies, "get the number of bodies in the coupling")
    .def("getIdList", &FoamCoupling::getIdList, "get the ids of bodies in coupling")
    .def("getFluidDomainBbox", &FoamCoupling::getFluidDomainBbox, "get the fluid domain bounding boxes, called once during simulation initialization. ")
	.def("SetOpenFoamSolver", &FoamCoupling::SetFoamSolver,(boost::python::arg("OpenFOAMSolverName"), boost::python::arg("numOpenFOAMProcesses")),"Starts the Yade coupled OpenFOAM solver with the requested number of processes")
	.def("StartFoamSolver",&FoamCoupling::StartFoamSolver,"Starts the OpenFOAM solver")

    .def_readonly("foamDeltaT", &FoamCoupling::foamDeltaT, "timestep in openfoam solver from  :yref:`exchangeDeltaT <FoamCoupling::exchangeDeltaT>` ")
    .def_readonly("dataExchangeInterval", &FoamCoupling::dataExchangeInterval, "Number of iterations/substepping : for stability and to be in sync with fluid solver calculated in :yref:`exchangeDeltaT <FoamCoupling::exchangeDeltaT>`")
    .def_readwrite("isGaussianInterp", &FoamCoupling::isGaussianInterp, "switch for Gaussian interpolation of field varibles in openfoam. Uses  :yref:`sumHydroForce<FoamCoupling::sumHydroForce>` to obtain hydrodynamic force ")
    .add_property("comm",&FoamCoupling::getMyComm,&FoamCoupling::setMyComm,"Communicator to be used for MPI (converts mpi4py comm <-> c++ comm)")
    )
	// clang-format on
	DECLARE_LOGGER;
};
REGISTER_SERIALIZABLE(FoamCoupling);
/* a class for holding info on the min and max bounds of the fluid mesh. Each fluid proc has a domain minmax and */
class FluidDomainBbox : public Shape {
public:
	//std::vector<double> minMaxBuff; // a buffer to receive the min max during MPI_Recv.
	void setMinMax(const std::vector<double>& minmaxbuff)
	{
		if (minmaxbuff.size() != 6) {
			LOG_ERROR("incorrect minmaxbuff size. FAIL");
			return;
		}
		minBound[0] = minmaxbuff[0];
		minBound[1] = minmaxbuff[1];
		minBound[2] = minmaxbuff[2];
		maxBound[0] = minmaxbuff[3];
		maxBound[1] = minmaxbuff[4];
		maxBound[2] = minmaxbuff[5];
		minMaxisSet = true;
	}
	int domainRank; // rank of the fluid domain.
	virtual ~FluidDomainBbox() {};
	YADE_CLASS_BASE_DOC_ATTRS_CTOR_PY(
	        FluidDomainBbox,
	        Shape,
	        "The bounding box of a fluid grid from one OpenFOAM/YALES2 proc",
	        ((bool, minMaxisSet, false, , "flag to check if the min max bounds of this body are set."))(
	                (std::vector<Body::id_t>, bIds, std::vector<Body::id_t>(), , "ids of bodies intersecting with this subdomain, "))(
	                (Vector3r, minBound, Vector3r(-NaN, -NaN, -NaN), , "min bounds of the fluid grid "))(
	                (Vector3r, maxBound, Vector3r(NaN, NaN, NaN), , "max bounds of the fluid grid"))(
	                (bool, hasIntersection, false, , "if this Yade subdomain has intersection with this OpenFOAM/YALES2 subdomain")),
	        createIndex();
	        , );
	DECLARE_LOGGER;
	REGISTER_CLASS_INDEX(FluidDomainBbox, Shape);
};
REGISTER_SERIALIZABLE(FluidDomainBbox);
class Bo1_FluidDomainBbox_Aabb : public BoundFunctor {
public:
	void go(const shared_ptr<Shape>&, shared_ptr<Bound>&, const Se3r& se3, const Body*) override;
	FUNCTOR1D(FluidDomainBbox);
	YADE_CLASS_BASE_DOC(Bo1_FluidDomainBbox_Aabb, BoundFunctor, "creates/updates an :yref:`Aabb` of a :yref:`FluidDomainBbox`.");
};
REGISTER_SERIALIZABLE(Bo1_FluidDomainBbox_Aabb);
} // namespace yade
#endif
