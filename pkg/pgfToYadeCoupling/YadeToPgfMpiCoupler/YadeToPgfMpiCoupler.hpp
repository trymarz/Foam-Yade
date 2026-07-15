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
// (registered once in FoamCoupling.cpp). Only YadeToPgfMpiCoupler itself is a new
// registered serializable.
#ifdef YADE_MPI

#pragma once

// Brings in GlobalEngine, Body, Scene, Subdomain, Sphere, mpi.h and the
// shared FluidDomainBbox / Bo1_FluidDomainBbox_Aabb shape classes.
#include <pkg/openfoam/FoamCoupling.hpp>

namespace yade { // Cannot have #include directive inside.
class Scene;
class Subdomain;
class Interaction;
class BodyContainer;
class YadeToPgfMpiCoupler : public GlobalEngine {
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
		void syncTimestepsWithPgf();
		bool isCouplingTimestep();
        void castTerminate();
		void receiveLambdaDotFromPgf();
		void verifyAllParticlesWereLocatedByPgf();
		bool ifFluidDomain(const Body::id_t& );
		int ifSharedId(const Body::id_t& );
		bool checkSharedDomains(const int& );
		int stride;
		void clearFluidDomainParticleLists();
		void exchangeParticleDataWithPgf();
		void applyLambdaDotToParticles();
		void syncLambdaDotToMaster();
		void spawnPgfSolverViaMpi();
		void SetPgfSolver(const std::string &solverName, unsigned numProcs);

		void buildLocalIds();
		void syncTimestepsWithPgfParallel();
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
		virtual ~YadeToPgfMpiCoupler(){};

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
		void receiveMeshBoundingBoxesFromPgf();
		void findIntersections();
		bool ifDomainBodies(const shared_ptr<Body>& );
		void sendParticleKinematicsToPgf();
		void sendNumberOfOverlapingParticlesToPgfProcs();
		int commSzdff;
		int otherCommSz;
		void buildSharedIdsMap();
		int ifSharedIdMap(const Body::id_t& );
		bool initDone = false;
		void checkFoamVersion();

    YADE_CLASS_BASE_DOC_ATTRS_INIT_CTOR_PY(YadeToPgfMpiCoupler,GlobalEngine,
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
    .def("setIdList", &YadeToPgfMpiCoupler::setIdList,boost::python::arg("bodyIdlist"), "list of body ids in coupling. (links to :yref: `YadeToPgfMpiCoupler::bodyList` vector, used to build particle data sent to foam: pos, vel, radius.)")
    .def("killMPI", &YadeToPgfMpiCoupler::killMPI, "Destroy MPI, to be called at the end of the simulation, from :yref:`killMPI<YadeToPgfMpiCoupler::killMPI>`")
    .def("setNumParticles",&YadeToPgfMpiCoupler::setNumParticles,boost::python::arg("numparticles"),"number of particles in coupling")
    .def("insertBodyId", &YadeToPgfMpiCoupler::insertBodyId,boost::python::arg("newId"), "insert a new body id for coupling")
    .def("eraseId", &YadeToPgfMpiCoupler::eraseId,boost::python::arg("idToErase"), "remove a body from coupling")
    .def("getNumBodies", &YadeToPgfMpiCoupler::getNumBodies, "get the number of bodies in the coupling")
    .def("getIdList", &YadeToPgfMpiCoupler::getIdList, "get the ids of bodies in coupling")
    .def("receiveMeshBoundingBoxesFromPgf", &YadeToPgfMpiCoupler::receiveMeshBoundingBoxesFromPgf, "get the fluid domain bounding boxes, called once during simulation initialization. ")
	.def("SetPgfSolver", &YadeToPgfMpiCoupler::SetPgfSolver,(boost::python::arg("pgfSolverName"), boost::python::arg("numPgfProcesses")),"Sets the PGF (OpenFOAM) solver to spawn and the requested number of processes")
	.def("spawnPgfSolverViaMpi",&YadeToPgfMpiCoupler::spawnPgfSolverViaMpi,"Spawns the PGF (OpenFOAM) solver processes via MPI_Comm_spawn")

    .def_readonly("foamDeltaT", &YadeToPgfMpiCoupler::foamDeltaT, "timestep in openfoam solver from  :yref:`syncTimestepsWithPgf <YadeToPgfMpiCoupler::syncTimestepsWithPgf>` ")
    .def_readonly("dataExchangeInterval", &YadeToPgfMpiCoupler::dataExchangeInterval, "Number of iterations/substepping : for stability and to be in sync with fluid solver calculated in :yref:`syncTimestepsWithPgf <YadeToPgfMpiCoupler::syncTimestepsWithPgf>`")
    .add_property("comm",&YadeToPgfMpiCoupler::getMyComm,&YadeToPgfMpiCoupler::setMyComm,"Communicator to be used for MPI (converts mpi4py comm <-> c++ comm)")
    )
	// clang-format on
	DECLARE_LOGGER;
};
REGISTER_SERIALIZABLE(YadeToPgfMpiCoupler);
} // namespace yade
#endif
