// YADE-porousGasificationFoam minimal coupling module
//
// Derived from FoamCoupling.hpp (c) 2019 Deepak Kunhappan. This is the
// stripped, PGF-native engine: it exchanges only particle kinematics
// (YADE -> OF) and a per-particle lambdaDot shrink factor (OF -> YADE).
// No drag, no lift, no torques, no Gaussian interpolation -- the fluid
// does not push back on the particles. The DEM spheres are mechanical
// markers whose motion produces the smooth Us solid-velocity field and
// whose radii shrink under the OF-computed lambdaDot signal.
//
// FluidDomainBbox / Bo1_FluidDomainBbox_Aabb are NOT redefined here: they
// are shared with the legacy coupling and come in via FoamCoupling.hpp
// (registered once in FoamCoupling.cpp). Only PgfCoupling itself is a new
// registered serializable.
#ifdef YADE_MPI

#pragma once

// Brings in GlobalEngine, Body, Scene, Subdomain, Sphere, mpi.h and the
// shared FluidDomainBbox / Bo1_FluidDomainBbox_Aabb shape classes.
#include "FoamCoupling.hpp"

namespace yade { // Cannot have #include directive inside.
class Scene;
class Subdomain;
class Interaction;
class BodyContainer;
class PgfCoupling : public GlobalEngine {
private:
	// some variables for MPI_Send/Recv
	const int  sendTag = 500;
	int        rank, commSize;              // for serial Yade-OpenFOAM
	int        foamCommSize, worldCommSize; //
	MPI_Status status;
	int        szdff, localCommSize, localRank, worldRank;
	const int  TAG_GRID_BBOX  = 1001;
	const int  TAG_PRT_DATA   = 1002;
	const int  TAG_FORCE      = 1005; // OF -> YADE lambdaDot tag (same value as the legacy force tag)
	const int  TAG_SEARCH_RES = 1004;
	const int  yadeMaster     = 0;
	const int  TAG_SZ_BUFF    = 1003;
	const int  TAG_FLUID_DT   = 1050;
	const int  TAG_YADE_DT    = 1060;
	const int  TAG_SHARED_ID  = 1080;
	bool       serialYade     = false;
	MPI_Comm   FOAMCOMM;  // the communicator used to spawn the FOAM processes.
	MPI_Comm   INTRACOMM; // the merged communicator to communicate with the FOAM processes.
public:
	// clang-format off
		void setNumParticles(int);
		void setIdList(const std::vector<int>& );
		void killMPI();
		void updateProcList();
		void resetProcList();
		void exchangeDeltaT();
		bool exchangeData();
        void castTerminate();
		void getParticleForce();
		void verifyParticleDetection();
		bool ifFluidDomain(const Body::id_t& );
		int ifSharedId(const Body::id_t& );
		bool checkSharedDomains(const int& );
		int stride;
		void resetFluidDomains();
		void runCoupling();
		void setHydroForce();
		void syncLambdaDotToMaster();
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
		virtual ~PgfCoupling(){};

		std::vector<int> bodyList;  // 'global' all Ids across all procs which are in coupling. Used in serial mode  coupling.
		std::vector<std::pair<Body::id_t, std::vector<Body::id_t> > > sharedIds;
		std::vector<std::pair<int, std::map<int, int> > > sharedIdsMapIndx;
		std::vector<std::pair<int, std::vector<double>> > hForce;
		std::vector<std::pair<int, int> > inCommunicationProc;
		std::vector<Body::id_t> localIds; // 'local', those Ids in the present subdomain  that are in coupling, used in parallel mode.



		std::string foamSolverName;

		int numFoamProcs;

		Real foamDeltaT;

		long int  dataExchangeInterval=1;
		bool recvdFoamDeltaT;
		void getFluidDomainBbox();
		void findIntersections();
		bool ifDomainBodies(const shared_ptr<Body>& );
		void sendBodyData();
		void sendIntersectionToFluidProcs();
		int commSzdff;
		int otherCommSz;
		void buildSharedIdsMap();
		int ifSharedIdMap(const Body::id_t& );
		bool initDone = false;
		void checkFoamVersion();

    YADE_CLASS_BASE_DOC_ATTRS_INIT_CTOR_PY(PgfCoupling,GlobalEngine,
   "A minimal engine for coupling Yade with porousGasificationFoam. Exchanges "
   "particle positions/velocities/radii (YADE->OF) and a per-particle lambdaDot "
   "shrink factor (OF->YADE). No drag, no lift, no torques -- the fluid does not "
   "push back on particles. The spheres are mechanical markers: they move under "
   "Yade's own physics and the OF-computed lambdaDot drives changeRadius().",
    ((int,numParticles,1, , "number of particles in coupling."))
    ((int,foamVersion,-1,,"version of OpenFoam environment"))
    ((string,foamPath,"",,"path to OpenFoam"))
    ((double,particleDensity,1, , "particle Density")) //not needed  as this is set in foam
    ((double,fluidDensity,1, ,"fluidDensity")) //not needed  as this is set in foam
    ((std::vector<Body::id_t>,fluidDomains, std::vector<Body::id_t>(),,"list of fluid domain bounding fictitious fluid bodies that has the fluid mesh bounds"))
    ,
    ,
    checkFoamVersion();
    ,
    .def("setIdList", &PgfCoupling::setIdList,boost::python::arg("bodyIdlist"), "list of body ids in coupling. (links to :yref: `PgfCoupling::bodyList` vector, used to build particle data sent to foam: pos, vel, radius.)")
    .def("killMPI", &PgfCoupling::killMPI, "Destroy MPI, to be called at the end of the simulation, from :yref:`killMPI<PgfCoupling::killMPI>`")
    .def("setNumParticles",&PgfCoupling::setNumParticles,boost::python::arg("numparticles"),"number of particles in coupling")
    .def("insertBodyId", &PgfCoupling::insertBodyId,boost::python::arg("newId"), "insert a new body id for coupling")
    .def("eraseId", &PgfCoupling::eraseId,boost::python::arg("idToErase"), "remove a body from coupling")
    .def("getNumBodies", &PgfCoupling::getNumBodies, "get the number of bodies in the coupling")
    .def("getIdList", &PgfCoupling::getIdList, "get the ids of bodies in coupling")
    .def("getFluidDomainBbox", &PgfCoupling::getFluidDomainBbox, "get the fluid domain bounding boxes, called once during simulation initialization. ")
	.def("SetOpenFoamSolver", &PgfCoupling::SetFoamSolver,(boost::python::arg("OpenFOAMSolverName"), boost::python::arg("numOpenFOAMProcesses")),"Starts the Yade coupled OpenFOAM solver with the requested number of processes")
	.def("StartFoamSolver",&PgfCoupling::StartFoamSolver,"Starts the OpenFOAM solver")

    .def_readonly("foamDeltaT", &PgfCoupling::foamDeltaT, "timestep in openfoam solver from  :yref:`exchangeDeltaT <PgfCoupling::exchangeDeltaT>` ")
    .def_readonly("dataExchangeInterval", &PgfCoupling::dataExchangeInterval, "Number of iterations/substepping : for stability and to be in sync with fluid solver calculated in :yref:`exchangeDeltaT <PgfCoupling::exchangeDeltaT>`")
    .add_property("comm",&PgfCoupling::getMyComm,&PgfCoupling::setMyComm,"Communicator to be used for MPI (converts mpi4py comm <-> c++ comm)")
    )
	// clang-format on
	DECLARE_LOGGER;
};
REGISTER_SERIALIZABLE(PgfCoupling);
} // namespace yade
#endif
