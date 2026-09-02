// (c) 2019  Deepak kunhappan : deepak.kunhappan@3sr-grenoble.fr; deepak.kn1990@gmail.com

#ifdef YADE_MPI

#include "FoamCoupling.hpp"
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpragmas"
#pragma GCC diagnostic ignored "-Wsuggest-override"
#pragma GCC diagnostic ignored "-Wcast-function-type"
#include <cstdlib>
#include <mpi.h>
#pragma GCC diagnostic pop

#include <pkg/common/Box.hpp>
#include <pkg/common/Facet.hpp>
#include <pkg/common/Grid.hpp>
#include <pkg/common/InsertionSortCollider.hpp>
#include <pkg/common/Sphere.hpp>

namespace yade { // Cannot have #include directive inside.

CREATE_LOGGER(FoamCoupling);
YADE_PLUGIN((FoamCoupling));
YADE_PLUGIN((FluidDomainBbox));
CREATE_LOGGER(FluidDomainBbox);
YADE_PLUGIN((Bo1_FluidDomainBbox_Aabb));


void Bo1_FluidDomainBbox_Aabb::go(const shared_ptr<Shape>& cm, shared_ptr<Bound>& bv, const Se3r&, const Body*)
{
	FluidDomainBbox* domain = static_cast<FluidDomainBbox*>(cm.get());
	if (!bv) {
		bv = shared_ptr<Bound>(new Aabb);
	}
	Aabb* aabb = static_cast<Aabb*>(bv.get());
	aabb->min  = scene->isPeriodic ? scene->cell->wrapPt(domain->minBound) : domain->minBound;
	aabb->max  = scene->isPeriodic ? scene->cell->wrapPt(domain->maxBound) : domain->maxBound;
	return;
}

void FoamCoupling::SetFoamSolver(const std::string& solverName, unsigned numProcs)
{
	foamSolverName = solverName;
	numFoamProcs   = numProcs;
}

void FoamCoupling::StartFoamSolver()
{
	assert(initDone == false && "Coupling has been initialized!");
	assert(!foamSolverName.empty() && "OpenFOAM solver has not been set.");
	//if (foamSolverName.empty()) LOG_ERROR("DasteXar : OpenFoam environment not found. Is environment variable WM_PROJECT_DIR defined?")
	scene = Omega::instance().getScene().get();
	// local rank and sizes
	MPI_Comm_rank(selfComm(), &localRank);
	MPI_Comm_size(selfComm(), &localCommSize);

	// spawn the openfoam process with the requested solver.
	// char *solverName = const_cast<char*>(foamSolverName.data());

	if (localRank == 0) std::cout << "Starting OpenFOAM solver --> " << foamSolverName << "  with number of processes = " << numFoamProcs << "\n";
	std::string par = "-parallel";

	char** argVs = new char*[2];
	argVs[0]     = const_cast<char*>(par.c_str());
	argVs[1]     = NULL;

	MPI_Comm_spawn(foamSolverName.data(), argVs, numFoamProcs, MPI_INFO_NULL, 0, MPI_COMM_WORLD, &FOAMCOMM, MPI_ERRCODES_IGNORE);
	if (localRank == 0)
		std::cout << "OpenFOAM sovler " << foamSolverName << "  started"
		          << "\n";

	// merge the openfoam child communicator for communications.
	MPI_Intercomm_merge(FOAMCOMM, 0, &INTRACOMM); // all openfoam communications are now done on INTRACOMM.
	std::cout << "comms merged" << std::endl;

	MPI_Comm_size(INTRACOMM, &worldCommSize);

	MPI_Comm_remote_size(FOAMCOMM, &foamCommSize);

	// sanity check of the FOAMCOMM and numFoamProcs
	if (localRank == 0) {
		assert(foamCommSize == numFoamProcs && "possible MPI error, difference in FoamCommunicator size and number of OpenFOAM processes.");
		assert(worldCommSize == foamCommSize + localCommSize && "invalid intracommunicator size");
	}

	//
	if (localCommSize == 1) serialYade = true;
	stride = localCommSize;
	// initiate the communication with OF, get the mesh bounding boxes from openfoam.
	getFluidDomainBbox();
	// FIXME!
	if (serialYade) {
		// use the collider once to get interactions between the bodies in coupling and the fluid domain boxes.
		// This is only needed in the serial case. In the parallel case, the collider is expliclty called
		// to create interactions between the SubDomains, bodies, and Fluid domains...
		// Better way to do this? or is there a way to 'set' interactions between specified pair of bodies?
		// Is this the right way to do?
		MPI_Comm_rank(MPI_COMM_WORLD, &rank);
		MPI_Comm_size(MPI_COMM_WORLD, &commSize);
		// const auto &collider = YADE_PTR_CAST<InsertionSortCollider>(scene->engineByName("InsertionSortCollider"));
		// collider->action();
	}
	std::cout << "got the fluid boxes and init done" << std::endl;
	initDone = true;
}


void FoamCoupling::setNumParticles(int np) { numParticles = np; }

void FoamCoupling::setIdList(const std::vector<int>& alist)
{
	bodyList.clear();
	bodyList.resize(alist.size());
	for (unsigned int i = 0; i != bodyList.size(); ++i) {
		bodyList[i] = alist[i];
	}
	bodyListModified = true;
}


void FoamCoupling::insertBodyId(int bId)
{
	const auto& iter = std::find(bodyList.begin(), bodyList.end(), bId);
	if (iter != bodyList.end()) {
		LOG_WARN("Body Id " << bId << "  already exists in coupling. ")
	} else {
		bodyList.push_back(bId);
	}
	bodyListModified = true;
}

bool FoamCoupling::eraseId(int bId)
{
	auto it = std::find(bodyList.begin(), bodyList.end(), bId);
	if (it != bodyList.end()) {
		bodyList.erase(it);
		return true;
	} else {
		LOG_ERROR("Id not found in list of ids in coupling");
		return false;
	}
	bodyListModified = true;
}


int FoamCoupling::getNumBodies() { return bodyList.size(); }

std::vector<int> FoamCoupling::getIdList() { return bodyList; }

void FoamCoupling::exchangeDeltaT()
{
	// Recv foamdt  first and broadcast;
	MPI_Recv(&foamDeltaT, 1, MPI_DOUBLE, 1, TAG_FLUID_DT, MPI_COMM_WORLD, &status);
	//bcast yadedt to others.
	Real yadeDt = scene->dt;
	MPI_Bcast(&yadeDt, 1, MPI_DOUBLE, rank, INTRACOMM);
	// calculate the interval . TODO: to include hydrodynamic time scale if inertial in openfoam
	// here -> hDeltaT = getViscousTimeScale();
	dataExchangeInterval = (long int)((yadeDt < foamDeltaT) ? foamDeltaT / yadeDt : 1);
}

Real FoamCoupling::getViscousTimeScale()
{
	//  Real hDeltaT = 0.0;
	//  Real dummy = 1e9;
	//
	//  MPI_Allreduce(&dummy, &hDeltaT, MPI_DOUBLE, MPI_MIN, MPI_COMM_WORLD);
	//
	return 0;
}


void FoamCoupling::getFluidDomainBbox()
{
	
	std::vector<std::vector<double>> minMaxBuff;

	//alloc memory
	for (int i = 0; i != foamCommSize; ++i) {
		std::vector<double> buff(6, 1e-50);
		minMaxBuff.push_back(buff);
	}

	//recv the grid minmax from each rank from foam solver. The FOAM ranks are 'higher', i.e. in the INTRACOMM, they follow after the Yade ranks.
	for (int rnk = 0; rnk != foamCommSize; ++rnk) {
		// MPI_Status status;
		std::vector<double>& buff = minMaxBuff[rnk];
		MPI_Recv(&buff.front(), 6, MPI_DOUBLE, rnk + stride, TAG_GRID_BBOX, INTRACOMM, &status);
	}

	//
	fluidDomains.resize(foamCommSize);
	//create fluidDomainBbox bodies and get their ids.
	for (int fd = 0; fd != foamCommSize; ++fd) {
		shared_ptr<Body>            flBody(shared_ptr<Body>(new Body()));
		shared_ptr<FluidDomainBbox> flBodyshape(shared_ptr<FluidDomainBbox>(new FluidDomainBbox()));
		flBodyshape->setMinMax(minMaxBuff[fd]);
		flBodyshape->domainRank      = stride + fd;
		flBodyshape->hasIntersection = false;
		flBody->shape                = flBodyshape;
		//if (!serialYade) {flBody->subdomain = 0; } else {flBody->subdomain=1;} // simply assigning a  dummy so that it works in serial case ( single yade proc)
		flBody->setIsFluidDomainBbox(true);
		fluidDomains[fd] = scene->bodies->insert(flBody);
	}
}

void FoamCoupling::buildSharedIdsMap()
{
	/*Builds the list of ids interacting with a fluid subdomain and stores those body ids that has intersections with several fluid domains.
	 sharedIdsMapIndx = a vector of std::pair<Body::id_t, std::map<fluidDomainId, indexOfthebodyinthefluidDomain aka index in flbdy-> bIds> > */

	inCommunicationProc.clear();

	// const shared_ptr<Subdomain>& subd = YADE_PTR_CAST<Subdomain>((*scene->bodies)[scene->thisSubdomainId]->shape); //not needed as we have localIds list.
	for (const auto& bodyId : localIds) {
		std::map<int, int> testMap;
		const auto&        bIntrs = (*scene->bodies)[bodyId]->intrs;
		for (const auto& itIntr : bIntrs) {
			const shared_ptr<Interaction>& intr = itIntr.second;
			Body::id_t                     otherId;
			if (bodyId == intr->getId1()) {
				otherId = intr->getId2();
			} else {
				otherId = intr->getId1();
			}
			const auto& otherBody = Body::byId(otherId, scene);
			if (otherBody->getIsFluidDomainBbox()) {
				const shared_ptr<FluidDomainBbox>& flbox = YADE_PTR_CAST<FluidDomainBbox>(otherBody->shape);
				flbox->bIds.push_back(bodyId);
				if (!flbox->hasIntersection) {
					flbox->hasIntersection = true;
				}
				int indx = (flbox->bIds.size()) - 1;
				testMap.insert(std::make_pair(
				        otherId,
				        indx)); // get the fluiddomainbbox body id and index in flbody->bIds, this will be used in the verifyTracking function
			}
		}
		if (testMap.size() > 1) { // this body has intersections with more than one fluid domains, hence this is a shared id .
			sharedIdsMapIndx.push_back(std::make_pair(bodyId, testMap));
		}
	}

	//for quickly identifying fluid procs.
	for (const auto& fluidId : fluidDomains) {
		const shared_ptr<Body>& flb = (*scene->bodies)[fluidId];
		if (flb) {
			const shared_ptr<FluidDomainBbox>& flBox = YADE_PTR_CAST<FluidDomainBbox>(flb->shape);
			if (flBox->bIds.size() > 0) {
				inCommunicationProc.push_back(std::make_pair(flBox->domainRank, flBox->bIds.size()));
			}
		}
	}
}


int FoamCoupling::ifSharedIdMap(const Body::id_t& testId)
{
	int res = -1;
	//std::vector<std::pair<int, std::map<int, int> > >::iterator
	auto it = std::find_if(sharedIdsMapIndx.begin(), sharedIdsMapIndx.end(), [&testId](const std::pair<int, std::map<int, int>> elem) -> bool {
		return testId == elem.first;
	});

	if (it != sharedIdsMapIndx.end()) {
		res = it - sharedIdsMapIndx.begin();
	}
	return res;
}


void FoamCoupling::buildLocalIds()
{
	//if (localRank==yadeMaster and not serialYade) { return; }  // master has no bodies.
	if (bodyList.size() == 0) {
		LOG_ERROR("Ids for coupling has no been set, FAIL!");
		return;
	}
	if (!serialYade) {
		if (localRank > yadeMaster) {
			const shared_ptr<Subdomain>& subD = YADE_PTR_CAST<Subdomain>(scene->subD);
			if (!subD) {
				LOG_ERROR("subdomain not found for local rank/ world rank  = " << localRank << "   " << worldRank);
				return;
			}
			for (const auto& testId : bodyList) {
				std::vector<Body::id_t>::iterator iter = std::find(subD->ids.begin(), subD->ids.end(), testId); // can subD have ids sorted?
				if (iter != subD->ids.end()) {
					localIds.push_back(*iter);
				}
			}
		} else {
			return;
		}
	} else {
		localIds = bodyList;
	}
}


bool FoamCoupling::ifDomainBodies(const shared_ptr<Body>& b)
{
	// check if body is subdomain, wall, facet, or other fluidDomainBbox

	shared_ptr<Box>             boxShape   = YADE_PTR_DYN_CAST<Box>(b->shape);
	shared_ptr<FluidDomainBbox> fluidShape = YADE_PTR_DYN_CAST<FluidDomainBbox>(b->shape);
	shared_ptr<Facet>           facetShape = YADE_PTR_DYN_CAST<Facet>(b->shape);

	if (b->getIsSubdomain()) {
		return true;
	} else if (boxShape) {
		return true;
	} else if (facetShape) {
		return true;
	} else {
		return false;
	}
}

void FoamCoupling::sendIntersectionToFluidProcs()
{
	// notify the fluid procs about intersection based on number of intersecting bodies.
	// vector of sendRecvRanks, with each vector element containing the number of bodies, if no bodies, send negative val.
	std::vector<int> sendRecvRanks(fluidDomains.size(), -1);
	for (unsigned f = 0; f != fluidDomains.size(); ++f) {
		const shared_ptr<Body>& fdomain = (*scene->bodies)[fluidDomains[f]];
		if (fdomain) {
			const shared_ptr<FluidDomainBbox>& fluidBox = YADE_PTR_CAST<FluidDomainBbox>(fdomain->shape);
			if (fluidBox->bIds.size() > 0) {
				sendRecvRanks[f] = fluidBox->bIds.size();
			} else {
				sendRecvRanks[f] = -1;
			}
		} else {
			sendRecvRanks[f] = -1;
		}
	}
	//
	int buffSz = fluidDomains.size();


	for (int rnk = 0; rnk != foamCommSize; ++rnk) {
		MPI_Send(&sendRecvRanks.front(), buffSz, MPI_INT, rnk + stride, TAG_SZ_BUFF, INTRACOMM);
	}
}

void FoamCoupling::sendBodyData()
{
	/* send the particle data to the associated fluid procs. prtData -> pos, vel, angvel, radius (for sphere), lambda, if fiber -> ori
	 * Stride is 11 doubles per particle. It must match the unpack stride in the
	 * OpenFOAM-side FoamYade::locateAllParticles()/allocArrays(): there is no
	 * buffer-layout negotiation over MPI (the only handshake is the particle
	 * count), so both binaries have to be built from matching revisions. */
	bool isPeriodic = scene->isPeriodic;
	for (int f = 0; f != static_cast<int>(fluidDomains.size()); ++f) {
		const shared_ptr<Body>& flbody = (*scene->bodies)[fluidDomains[f]];
		if (flbody) {
			const shared_ptr<FluidDomainBbox> flbox = YADE_PTR_CAST<FluidDomainBbox>(flbody->shape);
			if (flbox->hasIntersection) {
				std::vector<double> prtData(11 * flbox->bIds.size(), 1e-50);
				for (unsigned int i = 0; i != flbox->bIds.size(); ++i) {
					const shared_ptr<Body>& b = (*scene->bodies)[flbox->bIds[i]];
					if (isPeriodic) {
						const Vector3r& pos = scene->cell->wrapPt(b->state->pos);
						prtData[11 * i]     = pos[0];
						prtData[11 * i + 1] = pos[1];
						prtData[11 * i + 2] = pos[2];

					} else {
						prtData[11 * i]     = b->state->pos[0];
						prtData[11 * i + 1] = b->state->pos[1];
						prtData[11 * i + 2] = b->state->pos[2];
					}
					prtData[11 * i + 3] = b->state->vel[0];
					prtData[11 * i + 4] = b->state->vel[1];
					prtData[11 * i + 5] = b->state->vel[2];
					prtData[11 * i + 6] = b->state->angVel[0];
					prtData[11 * i + 7] = b->state->angVel[1];
					prtData[11 * i + 8] = b->state->angVel[2];

					const shared_ptr<Sphere>& sph = YADE_PTR_CAST<Sphere>(b->shape);
					prtData[11 * i + 9]           = sph->radius;

					// Reverse (Yade -> OpenFOAM) channel for the integrated
					// particle length scale, the counterpart of the lambdaDot
					// arriving through hydroForceBuff. OpenFOAM interpolates it
					// onto the mesh as the lambda field.
					prtData[11 * i + 10] = b->state->lambda_;
				}
				int sz = prtData.size();
				MPI_Send(&prtData.front(), sz, MPI_DOUBLE, flbox->domainRank, TAG_PRT_DATA, INTRACOMM);
			}
		}
	}
}


void FoamCoupling::verifyParticleDetection()
{
	/* check if the sent particles are located on the fluid procs, verify all particles (in fluid coupling) owned by the yade process has been accounted for.
	Some particles may intersect the  fluid domains bounding box but may not be actually inside the fluid mesh.
	 Method : Everty fluid proc sents a vector of it's search result. if found res = 1, else res =0, for each particle.
	 each yade rank receives this vector from intersecting fluid ranks, looks through the vector to find the fails.
	 if fail is found : see if this id is a sharedid. if not this particle has been 'lost'. if shared id :  check the vector of verifyTracking of the intersecting fluid domain till found in
	 at least one intersecting fluid box. if not particle has been lost. */

	//std::map<int, std::vector<int> > verifyTracking;  //vector containing domainRank, vector of "found/misses" for each body, miss = -1, found = 1.

	std::vector<std::pair<int, std::vector<int>>> verifyTracking;

	for (const auto& proc : inCommunicationProc) {
		std::vector<int> vt(proc.second, -1);
		verifyTracking.push_back(std::make_pair(proc.first, std::move(vt)));
	}

	// recv the vec.
	for (auto& it : verifyTracking) {
		std::vector<int>& vt  = it.second;
		int               rnk = it.first;
		// MPI_Status status;
		int buffSz = vt.size();
		MPI_Recv(&vt.front(), buffSz, MPI_INT, rnk, TAG_SEARCH_RES, INTRACOMM, &status);
	}


	//check for misses
	std::vector<Body::id_t> unFoundSharedIds;
	for (const auto& vt : verifyTracking) {
		const int&                         flBdyIndx = abs(vt.first - stride);
		const shared_ptr<FluidDomainBbox>& flbody    = YADE_PTR_CAST<FluidDomainBbox>((*scene->bodies)[fluidDomains[flBdyIndx]]->shape);
		int                                bIndx     = 0;
		for (const auto& val : vt.second) {
			if (val < 0) {
				// this body was not found in the fluid domain.
				const Body::id_t& testId = (*scene->bodies)[flbody->bIds[bIndx]]->id;
				// check if this body is a sharedId  from sharedIdsMap.
				int sharedIndx = ifSharedIdMap(testId);
				if (sharedIndx < 0) {
					const Vector3r& pos = (*scene->bodies)[testId]->state->pos;
					LOG_ERROR(
					        "Particle ID  = " << testId << " pos = " << pos[0] << " " << pos[1] << " " << pos[2]
					                          << " was not found in fluid domain"
					                          << "lost Particle in proc = " << localRank);
				} else {
					Body::id_t unfoundId = testId;
					unFoundSharedIds.push_back(unfoundId);
				}
			}
			++bIndx;
		}
	}

	//check if the 'sharedIds' has been located in any of the fluid procs.  (REWRITE FROM HERE)
	if (unFoundSharedIds.size() > 0) {
		for (const auto& idPair : sharedIdsMapIndx) {
			const auto& bodyId     = idPair.first;
			int         foundCount = 0;
			//const int mpSz = idPair.second.size();
			bool found = false;
			for (const auto& fdIndx : idPair.second) {
				const shared_ptr<FluidDomainBbox>& flbox = YADE_PTR_CAST<FluidDomainBbox>((*scene->bodies)[fdIndx.first]->shape);
				for (const auto& vt : verifyTracking) {
					if (vt.first == flbox->domainRank) {
						if (vt.second[fdIndx.second] > 0) {
							found = true;
							++foundCount;
							//  uncomment me for debugging..
							//   std::cout << "foundcount for bId = " <<  idPair.first <<  "  " << foundCount << std::endl;
							//   std::cout << "totsize for bId = " <<  idPair.first <<  "  " << idPair.second.size() << std::endl;
						}
					}
				}
			}
			if (!found) {
				const Vector3r& pos = (*scene->bodies)[bodyId]->state->pos;
				LOG_ERROR(
				        "Particle ID (SHARED ID )  = " << bodyId << " pos = " << pos[0] << " " << pos[1] << " " << pos[2]
				                                       << " was not found in fluid domain"
				                                       << " lost particle in proc " << localRank);
			}
		}
	}

	// 	for (auto& idPair : sharedIdsMapIndx){
	// 		for (auto iter = idPair.second.cbegin(); iter != idPair.second.cend();){
	// 			auto fdIndx = *iter;
	// 			const shared_ptr<FluidDomainBbox>& flbox = YADE_PTR_CAST<FluidDomainBbox>((*scene->bodies)[fdIndx.first]->shape);
	// 			for (const auto& vt : verifyTracking){
	// 				if (vt.first == flbox->domainRank){
	// 					if (vt.second[fdIndx.second] > 0) {
	// 					  ; ++iter;}
	// 				} else {
	// 					idPair.second.erase(iter++); // remove proc and indx from sharedIdsMap
	// 				}
	// 			}
	// 		}
	// 	}


	// 	for (const auto& idPair : sharedIdsMapIndx){
	// 		if (!idPair.second.size()) {
	// 			const auto& bodyId = idPair.first;
	// 			const Vector3r& pos = (*scene->bodies)[bodyId]->state->pos;
	// 			LOG_ERROR("Particle ID (SHARED ID )  = " << bodyId << " pos = " << pos[0] << " " << pos[1] << " " << pos[2] <<  " was not found in fluid domain" << " lost particle in proc " << localRank);
	// 		}
	// 	}

	std::vector<int>                           sharedIdsCount(inCommunicationProc.size(), 0);
	std::vector<std::vector<std::vector<int>>> sharedBuff;
	sharedBuff.resize(inCommunicationProc.size());

	// 	//prepare to send shared ids
	for (int ii = 0; ii != (int)inCommunicationProc.size(); ++ii) {
		for (const auto& idPair : sharedIdsMapIndx) {
			const auto& indxMap = idPair.second;
			// if and only if the size of the map is > 1, the particle has been found in more than one foam procs.
			if (indxMap.size() > 1) {
				const auto& indx = findRankSharedIndxMap(indxMap, inCommunicationProc[ii].first);
				if (indx >= 0) {
					std::vector<int> buff;
					buff.push_back(indx); // index of the id in the fluid proc's lst of particles
					for (const auto& val : indxMap) {
						const auto& flbx = YADE_PTR_CAST<FluidDomainBbox>((*scene->bodies)[val.first]->shape);
						if (inCommunicationProc[ii].first == flbx->domainRank) continue;
						buff.push_back(val.first);
					}
					sharedBuff[ii].push_back(buff);
				}
			}
		}
	}
	// send info on number of 'shared' ids
	std::vector<MPI_Request> mpiReqs;
	for (unsigned i = 0; i != sharedBuff.size(); ++i) {
		int         buffSz = (int)sharedBuff[i].size();
		MPI_Request req;
		MPI_Isend(&buffSz, 1, MPI_INT, inCommunicationProc[i].first, TAG_SHARED_ID, INTRACOMM, &req);
		mpiReqs.push_back(req);
	}


	for (auto& req : mpiReqs) {
		// MPI_Status status;
		MPI_Wait(&req, &status);
	}

	mpiReqs.clear();


	for (unsigned i = 0; i != sharedBuff.size(); ++i) {
		if (!sharedBuff[i].size()) continue;
		for (unsigned j = 0; j != sharedBuff[i].size(); ++j) {
			MPI_Request       req;
			std::vector<int>& buff = sharedBuff[i][j];
			int               sz   = (int)buff.size();
			MPI_Isend(&buff.front(), sz, MPI_INT, inCommunicationProc[i].first, TAG_SHARED_ID, INTRACOMM, &req);
			mpiReqs.push_back(req);
		}
	}
	if (mpiReqs.size()) {
		for (auto& rq : mpiReqs) {
			// MPI_Status status;
			MPI_Wait(&rq, &status);
		}
		mpiReqs.clear();
	}
}


int FoamCoupling::findRankSharedIndxMap(const std::map<int, int>& tMap, const int& rnk)
{
	// if found, return the index of the particle
	for (const auto& prcIndx : tMap) {
		const auto& flbx = YADE_PTR_CAST<FluidDomainBbox>((*scene->bodies)[prcIndx.first]->shape);
		if (rnk == flbx->domainRank) return prcIndx.second; // return the particle index
	}
	return -1;
}


void FoamCoupling::getParticleForce()
{
	hForce.clear();
	for (const auto& proc : inCommunicationProc) {
		std::vector<double> forceVec(7 * proc.second, 0.0);  //DasteXar changed from 6 to 7
		hForce.push_back(std::make_pair(proc.first, std::move(forceVec)));
	}


	for (auto& recvForce : hForce) {
		std::vector<double>& tmpForce = recvForce.second;
		int                  recvRank = recvForce.first;
		int                  buffSz   = tmpForce.size();

		/* fluid procs having no particles (those in inCommunicationProc) will send 0 force, torque */
		MPI_Recv(&tmpForce.front(), buffSz, MPI_DOUBLE, recvRank, TAG_FORCE, INTRACOMM, &status);
	}
}


void FoamCoupling::resetFluidDomains()
{
	// clear the vector ids held fluidDomainBbox->bIds
	if (localRank == yadeMaster and not serialYade) {
		return;
	}
	for (unsigned f = 0; f != fluidDomains.size(); ++f) {
		const shared_ptr<Body>& fdomain = (*scene->bodies)[fluidDomains[f]];
		if (fdomain) {
			const shared_ptr<FluidDomainBbox>& fluidBox = YADE_PTR_CAST<FluidDomainBbox>(fdomain->shape);
			fluidBox->bIds.clear();
		}
	}
	sharedIdsMapIndx.clear();
	localIds.clear();
}


void FoamCoupling::setHydroForce()
{
	// add the force
	if (localRank == yadeMaster and not serialYade) {
		return;
	}
	for (const auto& rf : hForce) {
		int                                indx     = abs(rf.first - localCommSize);
		const std::vector<double>&         forceVec = rf.second;
		const shared_ptr<FluidDomainBbox>& flbox    = YADE_PTR_CAST<FluidDomainBbox>((*scene->bodies)[fluidDomains[indx]]->shape);
		for (unsigned int i = 0; i != flbox->bIds.size(); ++i) {
			
			
			/* original 
			Vector3r fx;
			fx[0] = forceVec[6 * i];
			fx[1] = forceVec[6 * i + 1];
			fx[2] = forceVec[6 * i + 2];
			Vector3r tx;
			tx[0] = forceVec[6 * i + 3];
			tx[1] = forceVec[6 * i + 4];
			tx[2] = forceVec[6 * i + 5];
			scene->forces.addForce(flbox->bIds[i], fx);
			scene->forces.addTorque(flbox->bIds[i], tx);
			*/
			
			
			int base = 7 * i;

			// forces
			Vector3r fx;
			fx[0] = forceVec[base + 0];
			fx[1] = forceVec[base + 1];
			fx[2] = forceVec[base + 2];

			// torques
			Vector3r tx;
			tx[0] = forceVec[base + 3];
			tx[1] = forceVec[base + 4];
			tx[2] = forceVec[base + 5];

			// lambdaDot: rate of change of lambda [m/s], the forward counterpart
			// of the lambda_ sent back in sendBodyData()
			double lambdaDot = forceVec[base + 6];

			// assign inside YADE body state
			//scene->bodies[ flbox->bIds[i] ]->state->lambdaDot = lambdaDot;
			Body::byId(flbox->bIds[i], scene)->state->lambdaDot = lambdaDot;


			// add forces into YADE
			scene->forces.addForce(flbox->bIds[i], fx);
			scene->forces.addTorque(flbox->bIds[i], tx);

		}
	}
}

void FoamCoupling::exchangeDeltaTParallel()
{
	// Recv foamdt  first and broadcast;
	if (!serialYade) {
		if (localRank == yadeMaster) {
			// MPI_Status status;
			int fluidMaster = stride;
			MPI_Recv(&foamDeltaT, 1, MPI_DOUBLE, fluidMaster, TAG_FLUID_DT, INTRACOMM, &status);
		}

		//bcast  the fluidDt to all yade_procs.
		// Real  yadeDt = scene-> dt;
		MPI_Bcast(&foamDeltaT, 1, MPI_DOUBLE, yadeMaster, selfComm());

		//do a MPI_Allreduce (min) and get the minDt of all the yade procs..
		Real myDt = scene->dt;
		Real yadeDt;
		MPI_Allreduce(&myDt, &yadeDt, 1, MPI_DOUBLE, MPI_MIN, selfComm());


		// send the minDt to fluid proc (master .. )
		if (localRank == yadeMaster) {
			int fluidMaster = stride;
			MPI_Send(&yadeDt, 1, MPI_DOUBLE, fluidMaster, TAG_YADE_DT, INTRACOMM);
		}

		// calculate the interval . TODO: to include hydrodynamic time scale if inertial in openfoam
		// here -> hDeltaT = getViscousTimeScale();
		dataExchangeInterval = (long int)((yadeDt < foamDeltaT) ? foamDeltaT / yadeDt : 1);
	} else {
		exchangeDeltaT();
	}
}

void FoamCoupling::runCoupling()
{
	if (localRank > yadeMaster or serialYade) {
		buildLocalIds();
		buildSharedIdsMap();
		sendIntersectionToFluidProcs();
		sendBodyData();
		verifyParticleDetection();
		getParticleForce();
	}
}

void FoamCoupling::action()
{
	if (!initDone) {
		StartFoamSolver();
	}
	if (exchangeData()) {
		resetFluidDomains();
		runCoupling();
		exchangeDeltaTParallel();
	}
	setHydroForce();
}

bool FoamCoupling::exchangeData() { return scene->iter % dataExchangeInterval == 0; }


void FoamCoupling::killMPI()
{
	//  int value = 1;  // Use the same value as in OpenFOAM
	// MPI_Bcast(&value, 1, MPI_INT, 0, INTRACOMM);  // Broadcast from rank 0 in Yade

	// Now sync all processes

	//MPI_Barrier(INTRACOMM);
	// MPI_Finalize();  // Finalize MPI
	MPI_Abort(INTRACOMM, -100);
}

void FoamCoupling::checkFoamVersion()
{
	const char* wmProjectDir = std::getenv("WM_PROJECT_DIR");
	if (wmProjectDir != nullptr) {
		foamPath = std::string(wmProjectDir);
		string version(std::getenv("WM_PROJECT_VERSION"));
		if (!version.empty() && version.front() == 'v') //remove the leading 'v'
			version.erase(0, 1);
		foamVersion = std::stoi(version);
	}
}


} // namespace yade

#endif
