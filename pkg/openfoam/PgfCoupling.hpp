// Minimal Yade side of the porousGasificationFoam (PGF) coupling (PGF #271).
//
// Wire protocol, version 1. Must match pgfYadeCoupler in PGF
// (porousGasificationMedia/DEM/pgfYadeCoupler); the startup handshake checks
// the version, both strides and lambdaMode, and stops by name on a mismatch.
//   handshake  OF rank -> every Yade rank: HELLO int[6] {magic, version,
//              strideUp, strideDown, lambdaMode, nFoamRanks}, BBOX double[6];
//              Yade rank 0 -> every OF rank: ACK int[6] {magic, version,
//              status, firstWorker, nWorkers, 0}.
//   exchange   Yade worker -> every OF rank: COUNT int n, DATA double[n*8]
//              {bodyId, pos[3], vel[3], lambda_};
//              OF rank -> every Yade worker: REPLY_COUNT int m, REPLY
//              double[m*2] {bodyId, value}, only for the bodies it found;
//              OF rank 0 -> Yade rank 0: FOAM_DT double.
// value is lambdaDot [m/s] (lambdaMode 0) or the apparentVolume factor r
// (lambdaMode 1). No drag, torque, radius or angular velocity crosses.
#ifdef YADE_MPI

#pragma once

#include <lib/base/Logging.hpp>
#include <lib/serialization/Serializable.hpp>
#include <core/Body.hpp>
#include <core/GlobalEngine.hpp>
#include <core/Omega.hpp>
#include <core/Scene.hpp>
#include <core/Subdomain.hpp>
#include <array>
#include <string>
#include <vector>
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpragmas"
#pragma GCC diagnostic ignored "-Wsuggest-override"
#pragma GCC diagnostic ignored "-Wcast-function-type"
#include <mpi.h>
#pragma GCC diagnostic pop

namespace yade { // Cannot have #include directive inside.

class PgfCoupling : public GlobalEngine {
public:
	// clang-format off
	static constexpr int PROTOCOL_VERSION = 1;
	static constexpr int MAGIC            = 0x50474659; // "PGFY"
	static constexpr int STRIDE_UP        = 8;          // Yade -> PGF doubles per body
	static constexpr int STRIDE_DOWN      = 2;          // PGF -> Yade doubles per body
	static constexpr int TAG_HELLO        = 2701;
	static constexpr int TAG_BBOX         = 2702;
	static constexpr int TAG_ACK          = 2703;
	static constexpr int TAG_COUNT        = 2711;
	static constexpr int TAG_DATA         = 2712;
	static constexpr int TAG_REPLY_COUNT  = 2713;
	static constexpr int TAG_REPLY        = 2714;
	static constexpr int TAG_FOAM_DT      = 2721;
	static constexpr int TAG_LEGACY_BBOX  = 1001; // what legacy FoamYade sends first

	enum Status { OK = 0, BAD_VERSION = 1, BAD_STRIDE = 2, BAD_LAMBDA_MODE = 3, BAD_MAGIC = 4 };

private:
	MPI_Comm   FOAMCOMM  = MPI_COMM_NULL;
	MPI_Comm   INTRACOMM = MPI_COMM_NULL;
	MPI_Comm*  myComm_p  = nullptr;
	int        localRank = 0, localCommSize = 1, foamCommSize = 0;
	bool       serialYade = true;
	bool       initDone   = false;
	std::vector<std::array<double, 6>> foamBoxes;   // per OF rank: min[3], max[3]
	std::vector<std::vector<Body::id_t>> sentIds;   // per OF rank, this exchange

	MPI_Comm selfComm() const { return myComm_p ? *myComm_p : MPI_COMM_WORLD; }
	int  foamRank(int r) const { return localCommSize + r; } // OF rank r in INTRACOMM
	bool isWorker() const { return serialYade or localRank > 0; }
	[[noreturn]] void fail(const std::string& msg) const;
	void waitFor(int source, int tag, double timeout, const std::string& what) const;
	void startFoamSolver();
	void handshake();
	void sendBodies();
	void receiveAndApply();
	void exchangeDeltaT();
	void setExchangeInterval(Real yadeDt);
	bool inBox(const Vector3r& p, int r) const;

public:
	void setIdList(const std::vector<int>& ids) { bodyList = ids; }
	void insertBodyId(int id);
	bool eraseId(int id);
	std::vector<int> getIdList() const { return bodyList; }
	int  getNumBodies() const { return (int)bodyList.size(); }
	void setFoamSolver(const std::string& name, unsigned nProcs) { foamSolverName = name; numFoamProcs = nProcs; }
	void setMyComm(boost::python::object py_comm)
	{
		if (import_mpi4py() < 0) return;
		myComm_p = PyMPIComm_Get(py_comm.ptr());
		if (myComm_p == NULL) LOG_ERROR("invalid COMM received from Python");
	}
	PyObject* getMyComm() { return PyMPIComm_New(selfComm()); }
	void action() override;
	virtual ~PgfCoupling() {};

	std::vector<int> bodyList;
	std::string      foamSolverName;
	unsigned         numFoamProcs = 1;

	YADE_CLASS_BASE_DOC_ATTRS_CTOR_PY(PgfCoupling, GlobalEngine,
	"Minimal coupling with porousGasificationFoam (PGF #271). Sends each coupled body's id, position, velocity and :yref:`State.lambda_` "
	"to the OpenFOAM rank whose mesh box holds its centre, and applies what that rank returns: a rate set as :yref:`State.lambdaDot` "
	"(lambdaMode 0) or a factor multiplied into :yref:`State.lambda_` (lambdaMode 1). No hydrodynamic force or torque is applied. "
	"Values are applied once per exchange, keyed by body id, so bodies erased between exchanges are skipped. Pair it with "
	"``coupler pgf;`` in OpenFOAM's constant/yadeProperties; the startup handshake stops by name on any mismatch.",
	((int, lambdaMode, -1, Attr::readonly, "Set by the handshake: 0 = rate, 1 = apparentVolume factor."))
	((int, expectLambdaMode, -1, , "If 0 or 1, the handshake stops unless OpenFOAM reports this lambdaMode; -1 accepts either."))
	((Real, handshakeTimeout, 3600, , "Seconds to wait for OpenFOAM's handshake (it follows mesh and field setup) before stopping."))
	((Real, foamDeltaT, 0, Attr::readonly, "OpenFOAM deltaT from the last exchange."))
	((long, dataExchangeInterval, 1, Attr::readonly, "DEM steps per exchange: foamDeltaT/O.dt (an exact integer in rate mode, #230)."))
	((int, nLost, 0, Attr::readonly, "Bodies sent at the last exchange that no OpenFOAM rank found."))
	,
	,
	.def("setIdList", &PgfCoupling::setIdList, boost::python::arg("bodyIdlist"), "Ids of the bodies in the coupling.")
	.def("insertBodyId", &PgfCoupling::insertBodyId, boost::python::arg("newId"), "Add a body to the coupling.")
	.def("eraseId", &PgfCoupling::eraseId, boost::python::arg("idToErase"), "Remove a body from the coupling.")
	.def("getIdList", &PgfCoupling::getIdList, "Ids of the bodies in the coupling.")
	.def("getNumBodies", &PgfCoupling::getNumBodies, "Number of bodies in the coupling.")
	.def("setNumParticles", +[](PgfCoupling&, int) {}, boost::python::arg("numparticles"), "Ignored; kept so FoamCoupling scripts run unchanged.")
	.def("SetOpenFoamSolver", &PgfCoupling::setFoamSolver, (boost::python::arg("OpenFOAMSolverName"), boost::python::arg("numOpenFOAMProcesses")), "OpenFOAM solver to spawn, and its rank count.")
	.add_property("comm", &PgfCoupling::getMyComm, &PgfCoupling::setMyComm, "Communicator to be used for MPI (converts mpi4py comm <-> c++ comm)")
	)
	// clang-format on
	DECLARE_LOGGER;
};
REGISTER_SERIALIZABLE(PgfCoupling);

} // namespace yade
#endif
