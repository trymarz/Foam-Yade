// Minimal Yade side of the porousGasificationFoam coupling; protocol in PgfCoupling.hpp.
#ifdef YADE_MPI

#include "PgfCoupling.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace yade { // Cannot have #include directive inside.

CREATE_LOGGER(PgfCoupling);
YADE_PLUGIN((PgfCoupling));

void PgfCoupling::fail(const std::string& msg) const
{
	std::cerr << "PgfCoupling: " << msg << std::endl;
	MPI_Abort(INTRACOMM != MPI_COMM_NULL ? INTRACOMM : MPI_COMM_WORLD, 1);
	std::abort();
}

void PgfCoupling::insertBodyId(int id)
{
	if (std::find(bodyList.begin(), bodyList.end(), id) == bodyList.end()) bodyList.push_back(id);
}

bool PgfCoupling::eraseId(int id)
{
	auto it = std::find(bodyList.begin(), bodyList.end(), id);
	if (it == bodyList.end()) return false;
	bodyList.erase(it);
	return true;
}

// Blocks until a message from source is pending; stops by name on a legacy
// FoamYade peer (its first message is the 1001 mesh box) or on timeout.
void PgfCoupling::waitFor(int source, int tag, double timeout, const std::string& what) const
{
	const auto start = std::chrono::steady_clock::now();
	for (;;) {
		int        flag = 0;
		MPI_Status st;
		MPI_Iprobe(source, MPI_ANY_TAG, INTRACOMM, &flag, &st);
		if (flag) {
			if (st.MPI_TAG == tag) return;
			if (st.MPI_TAG == TAG_LEGACY_BBOX)
				fail("OpenFOAM runs the legacy FoamYade coupler (it sent tag 1001). Set 'coupler pgf;' in constant/yadeProperties, "
				     "or use FoamCoupling() in the DEM script.");
			std::ostringstream m;
			m << "expected " << what << " (tag " << tag << ") from OpenFOAM, got tag " << st.MPI_TAG;
			fail(m.str());
		}
		const double waited = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
		if (waited > timeout) {
			std::ostringstream m;
			m << "no " << what << " from OpenFOAM within " << timeout << " s. Is 'coupler pgf;' set in constant/yadeProperties?";
			fail(m.str());
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
}

void PgfCoupling::startFoamSolver()
{
	scene = Omega::instance().getScene().get();
	MPI_Comm_rank(selfComm(), &localRank);
	MPI_Comm_size(selfComm(), &localCommSize);
	serialYade = (localCommSize == 1);
	// Only spawn's root reads the solver name; mpy workers get a copy without it.
	if (localRank == 0 and foamSolverName.empty()) fail("no OpenFOAM solver set; call SetOpenFoamSolver(name, nProcs) first");

	char  par[]   = "-parallel";
	char* argv[2] = { par, nullptr };
	if (localRank == 0) std::cout << "PgfCoupling: starting " << foamSolverName << " on " << numFoamProcs << " ranks" << std::endl;
	MPI_Comm_spawn(foamSolverName.c_str(), argv, (int)numFoamProcs, MPI_INFO_NULL, 0, MPI_COMM_WORLD, &FOAMCOMM, MPI_ERRCODES_IGNORE);
	MPI_Intercomm_merge(FOAMCOMM, 0, &INTRACOMM);
	MPI_Comm_remote_size(FOAMCOMM, &foamCommSize);
	handshake();
	initDone = true;
}

void PgfCoupling::handshake()
{
	foamBoxes.assign(foamCommSize, {});
	int mode = -1;
	for (int r = 0; r != foamCommSize; ++r) {
		std::array<int, 6> hello {};
		waitFor(foamRank(r), TAG_HELLO, handshakeTimeout, "handshake");
		MPI_Recv(hello.data(), 6, MPI_INT, foamRank(r), TAG_HELLO, INTRACOMM, MPI_STATUS_IGNORE);
		MPI_Recv(foamBoxes[r].data(), 6, MPI_DOUBLE, foamRank(r), TAG_BBOX, INTRACOMM, MPI_STATUS_IGNORE);

		std::ostringstream err;
		int                status = OK;
		if (hello[0] != MAGIC) {
			status = BAD_MAGIC;
			err << "OpenFOAM rank " << r << " sent no PGF handshake (bad magic)";
		} else if (hello[1] != PROTOCOL_VERSION) {
			status = BAD_VERSION;
			err << "protocol version mismatch: OpenFOAM pgfYadeCoupler v" << hello[1] << ", Yade PgfCoupling v" << PROTOCOL_VERSION;
		} else if (hello[2] != STRIDE_UP or hello[3] != STRIDE_DOWN) {
			status = BAD_STRIDE;
			err << "per-particle stride mismatch: OpenFOAM expects up/down " << hello[2] << "/" << hello[3] << ", Yade sends/reads "
			    << STRIDE_UP << "/" << STRIDE_DOWN;
		} else if (hello[4] != 0 and hello[4] != 1) {
			status = BAD_LAMBDA_MODE;
			err << "unknown lambdaMode " << hello[4] << " from OpenFOAM rank " << r;
		} else if (r > 0 and hello[4] != mode) {
			status = BAD_LAMBDA_MODE;
			err << "OpenFOAM ranks disagree on lambdaMode (" << mode << " vs " << hello[4] << ")";
		} else if (expectLambdaMode >= 0 and hello[4] != expectLambdaMode) {
			status = BAD_LAMBDA_MODE;
			err << "lambdaMode mismatch: OpenFOAM lambdaDict gives " << hello[4] << ", the DEM script expects " << expectLambdaMode;
		}
		mode = hello[4];
		if (status != OK) {
			if (localRank == 0) {
				std::array<int, 6> ack { MAGIC, PROTOCOL_VERSION, status, 0, 0, 0 };
				for (int q = 0; q != foamCommSize; ++q)
					MPI_Send(ack.data(), 6, MPI_INT, foamRank(q), TAG_ACK, INTRACOMM);
			}
			fail(err.str());
		}
	}
	lambdaMode = mode;

	if (localRank == 0) {
		const int          nWorkers = serialYade ? 1 : localCommSize - 1;
		std::array<int, 6> ack { MAGIC, PROTOCOL_VERSION, OK, serialYade ? 0 : 1, nWorkers, 0 };
		for (int r = 0; r != foamCommSize; ++r)
			MPI_Send(ack.data(), 6, MPI_INT, foamRank(r), TAG_ACK, INTRACOMM);
		std::cout << "PgfCoupling: handshake ok, protocol v" << PROTOCOL_VERSION << ", lambdaMode " << lambdaMode << ", "
		          << foamCommSize << " OpenFOAM ranks" << std::endl;
	}
}

bool PgfCoupling::inBox(const Vector3r& p, int r) const
{
	const auto& b = foamBoxes[r];
	for (int k = 0; k != 3; ++k) {
		const double tol = 1e-9 * std::max(1.0, std::abs(b[k + 3] - b[k]));
		if (p[k] < b[k] - tol or p[k] > b[k + 3] + tol) return false;
	}
	return true;
}

// Each worker sends every OF rank the owned bodies whose centre lies in that
// rank's mesh box. Erased ids (null bodies) are skipped, not dereferenced.
void PgfCoupling::sendBodies()
{
	sentIds.assign(foamCommSize, {});
	std::vector<std::vector<double>> buf(foamCommSize);
	for (const int id : bodyList) {
		if (id < 0 or !scene->bodies->exists(id)) continue;
		const shared_ptr<Body>& b = Body::byId(id, scene);
		if (!b or !b->state) continue;
		if (!serialYade and b->subdomain != scene->subdomain) continue;
		const Vector3r pos = scene->isPeriodic ? scene->cell->wrapPt(b->state->pos) : b->state->pos;
		for (int r = 0; r != foamCommSize; ++r) {
			if (!inBox(pos, r)) continue;
			sentIds[r].push_back(id);
			const Vector3r& v = b->state->vel;
			buf[r].insert(buf[r].end(), { double(id), pos[0], pos[1], pos[2], v[0], v[1], v[2], double(b->state->lambda_) });
		}
	}
	std::vector<int>         counts(foamCommSize);
	std::vector<MPI_Request> reqs;
	for (int r = 0; r != foamCommSize; ++r) {
		counts[r] = (int)sentIds[r].size();
		reqs.emplace_back();
		MPI_Isend(&counts[r], 1, MPI_INT, foamRank(r), TAG_COUNT, INTRACOMM, &reqs.back());
		if (counts[r] > 0) {
			reqs.emplace_back();
			MPI_Isend(buf[r].data(), (int)buf[r].size(), MPI_DOUBLE, foamRank(r), TAG_DATA, INTRACOMM, &reqs.back());
		}
	}
	MPI_Waitall((int)reqs.size(), reqs.data(), MPI_STATUSES_IGNORE);
}

// Only the rank that found a body sets its value (#229); a second claim is
// ignored. Values are applied here, once per exchange, keyed by body id.
void PgfCoupling::receiveAndApply()
{
	std::unordered_set<Body::id_t> applied;
	for (int r = 0; r != foamCommSize; ++r) {
		int m = 0;
		MPI_Recv(&m, 1, MPI_INT, foamRank(r), TAG_REPLY_COUNT, INTRACOMM, MPI_STATUS_IGNORE);
		if (m < 0 or m > (int)sentIds[r].size()) {
			std::ostringstream e;
			e << "OpenFOAM rank " << r << " returned " << m << " bodies, " << sentIds[r].size() << " were sent";
			fail(e.str());
		}
		if (m == 0) continue;
		std::vector<double> reply(STRIDE_DOWN * m);
		MPI_Recv(reply.data(), (int)reply.size(), MPI_DOUBLE, foamRank(r), TAG_REPLY, INTRACOMM, MPI_STATUS_IGNORE);
		const std::unordered_set<Body::id_t> sent(sentIds[r].begin(), sentIds[r].end());
		for (int i = 0; i != m; ++i) {
			const Body::id_t id    = (Body::id_t)std::llround(reply[STRIDE_DOWN * i]);
			const double     value = reply[STRIDE_DOWN * i + 1];
			if (!sent.count(id)) {
				std::ostringstream e;
				e << "OpenFOAM rank " << r << " returned body " << id << ", which was not sent to it";
				fail(e.str());
			}
			if (!applied.insert(id).second or !scene->bodies->exists(id)) continue;
			const shared_ptr<Body>& b = Body::byId(id, scene);
			if (!b or !b->state) continue;
			if (lambdaMode == 1) {
				if (value > 0) b->state->lambda_ *= value;
				b->state->lambdaDot = 0.0;
			} else {
				b->state->lambdaDot = value;
			}
		}
	}
	std::unordered_set<Body::id_t> sentAll;
	for (const auto& ids : sentIds)
		sentAll.insert(ids.begin(), ids.end());
	nLost = (int)(sentAll.size() - applied.size());
	if (nLost > 0) LOG_WARN("PgfCoupling: " << nLost << " coupled bodies were in no OpenFOAM rank's mesh at iter " << scene->iter);
}

void PgfCoupling::exchangeDeltaT()
{
	Real fdt = 0;
	if (localRank == 0) MPI_Recv(&fdt, 1, MPI_DOUBLE, foamRank(0), TAG_FOAM_DT, INTRACOMM, MPI_STATUS_IGNORE);
	MPI_Bcast(&fdt, 1, MPI_DOUBLE, 0, selfComm());
	foamDeltaT = fdt;
	Real myDt = scene->dt, yadeDt = scene->dt;
	MPI_Allreduce(&myDt, &yadeDt, 1, MPI_DOUBLE, MPI_MIN, selfComm());
	setExchangeInterval(yadeDt);
}

// Rate mode integrates lambda every DEM step, so yadeDt must divide foamDt (#230).
void PgfCoupling::setExchangeInterval(Real yadeDt)
{
	if (lambdaMode == 1) {
		dataExchangeInterval = (long)((yadeDt < foamDeltaT) ? foamDeltaT / yadeDt : 1);
		return;
	}
	const Real ratio = foamDeltaT / yadeDt;
	const long n     = std::lround(ratio);
	if (n < 1 or std::abs(ratio - n) > 1e-6 * n) {
		std::ostringstream e;
		e << "in a lambdaDot rate mode Yade's dt must divide the OpenFOAM deltaT exactly (yadeDt <= foamDt, foamDt/yadeDt an "
		     "integer); got yadeDt = "
		  << yadeDt << ", foamDt = " << foamDeltaT << ", ratio = " << ratio << ". Set O.dt to foamDt/N.";
		fail(e.str());
	}
	dataExchangeInterval = n;
}

void PgfCoupling::action()
{
	if (!initDone) startFoamSolver();
	if (scene->iter % dataExchangeInterval != 0) return;
	if (isWorker()) {
		sendBodies();
		receiveAndApply();
	}
	exchangeDeltaT();
}

} // namespace yade

#endif
