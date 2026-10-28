/*************************************************************************
 Copyright (C) 2008 by Bruno Chareyre		                         *
*  bruno.chareyre@grenoble-inp.fr      					 *
*                                                                        *
*  This program is free software; it is licensed under the terms of the  *
*  GNU General Public License v2 or later. See file LICENSE for details. *
*************************************************************************/

#include <lib/base/Math.hpp>
#include <lib/high-precision/Constants.hpp>
#include <lib/serialization/EnumSupport.hpp>
#include <core/Clump.hpp>
#include <core/Scene.hpp>
#include <pkg/dem/NewtonIntegrator.hpp>

namespace yade { // Cannot have #include directive inside.

YADE_ENUM(yade::NewtonIntegrator, RotAlgorithm, (delValle2023)(Omelyan1998)(Fincham1992));

using math::max;
using math::min; // using inside .cpp file is ok.

YADE_PLUGIN((NewtonIntegrator));
CREATE_LOGGER(NewtonIntegrator);

// 1st order numerical damping
void NewtonIntegrator::cundallDamp1st(Vector3r& force, const Vector3r& vel)
{
	for (int i = 0; i < 3; i++)
		force[i] *= 1 - damping * math::sign(force[i] * vel[i]);
}
// 2nd order numerical damping
void NewtonIntegrator::cundallDamp2nd(const Real& dt, const Vector3r& vel, Vector3r& accel)
{
	for (int i = 0; i < 3; i++)
		accel[i] *= 1 - damping * math::sign(accel[i] * (vel[i] + 0.5 * dt * accel[i]));
}


Vector3r NewtonIntegrator::computeAccelWithoutGravity(const Vector3r& force, const Real& mass, int blockedDOFs)
{
	if (blockedDOFs == 0) return (force / mass);
	Vector3r ret(Vector3r::Zero());
	for (int i = 0; i < 3; i++)
		if (!(blockedDOFs & State::axisDOF(i, false))) ret[i] += force[i] / mass;
	return ret;
}
Vector3r NewtonIntegrator::addGravity(int blockedDOFs)
{
	if (blockedDOFs == 0) return (gravity);
	Vector3r ret(Vector3r::Zero());
	for (int i = 0; i < 3; i++)
		if (!(blockedDOFs & State::axisDOF(i, false))) ret[i] = gravity[i];
	return ret;
}


Vector3r NewtonIntegrator::computeAccel(const Vector3r& force, const Real& mass, int blockedDOFs)
{
	if (blockedDOFs == 0) return (force / mass + gravity);
	Vector3r ret(Vector3r::Zero());
	for (int i = 0; i < 3; i++)
		if (!(blockedDOFs & State::axisDOF(i, false))) ret[i] += force[i] / mass + gravity[i];
	return ret;
}
Vector3r NewtonIntegrator::computeAngAccel(const Vector3r& torque, const Vector3r& inertia, int blockedDOFs)
{
	if (blockedDOFs == 0) return torque.cwiseQuotient(inertia);
	Vector3r ret(Vector3r::Zero());
	for (int i = 0; i < 3; i++)
		if (!(blockedDOFs & State::axisDOF(i, true))) ret[i] += torque[i] / inertia[i];
	return ret;
}

void NewtonIntegrator::updateEnergy(const shared_ptr<Body>& b, const State* state, const Vector3r& fluctVel, const Vector3r& f, const Vector3r& m)
{
	assert(b->isStandalone() || b->isClump());
	// always positive dissipation, by-component: |F_i|*|v_i|*damping*dt (|T_i|*|ω_i|*damping*dt for rotations)
	if (damping != 0. && state->isDamped) {
		scene->energy->add(
		        fluctVel.cwiseAbs().dot((f + state->mass * gravity).cwiseAbs()) * damping * scene->dt,
		        "nonviscDamp",
		        nonviscDampIx,
		        /*non-incremental*/ false);
		// when the aspherical integrator is used, torque is damped instead of ang acceleration; this code is only approximate
		scene->energy->add(state->angVel.cwiseAbs().dot(m.cwiseAbs()) * damping * scene->dt, "nonviscDamp", nonviscDampIx, false);
	}
	// kinetic energy
	Real Etrans = .5 * state->mass * fluctVel.squaredNorm();
	Real Erot;
	// rotational terms
	if (b->isAspherical()) {
		const Matrix3r mI = state->inertia.asDiagonal();
		Matrix3r       T  = state->ori.toRotationMatrix();
		Erot              = .5 * b->state->angVel.dot((T * mI * T.transpose()) * b->state->angVel);
	} else {
		Erot = 0.5 * state->angVel.dot(state->inertia.cwiseProduct(state->angVel));
	}
	if (!kinSplit) scene->energy->add(Etrans + Erot, "kinetic", kinEnergyIx, /*non-incremental*/ true);
	else {
		scene->energy->add(Etrans, "kinTrans", kinEnergyTransIx, true);
		scene->energy->add(Erot, "kinRot", kinEnergyRotIx, true);
	}
	// gravitational work (work done by gravity is "negative", since the energy appears in the system from outside)
	scene->energy->add(-gravity.dot(b->state->vel) * b->state->mass * scene->dt, "gravWork", fieldWorkIx, /*non-incremental*/ false);
}

void NewtonIntegrator::saveMaximaVelocity(const Body::id_t& /*id*/, State* state)
{
#ifdef YADE_OPENMP
	Real& thrMaxVSq = threadMaxVelocitySq[omp_get_thread_num()];
	thrMaxVSq       = max(thrMaxVSq, state->vel.squaredNorm());
#else
	maxVelocitySq = max(maxVelocitySq, state->vel.squaredNorm());
#endif
}


void NewtonIntegrator::saveMaximaDisplacement(const shared_ptr<Body>& b)
{
	if (!b->bound) return; //clumps for instance, have no bounds, hence not saved
	Vector3r disp    = b->state->pos - b->bound->refPos;
	Real     maxDisp = max(math::abs(disp[0]), max(math::abs(disp[1]), math::abs(disp[2])));
	if (!maxDisp
	    || maxDisp < b->bound->sweepLength) { /*b->bound->isBounding = (updatingDispFactor>0 && (updatingDispFactor*maxDisp)<b->bound->sweepLength);*/
		maxDisp = 0.5;                    //not 0, else it will be seen as "not updated" by the collider, but less than 1 means no colliding
	} else {                                  /*b->bound->isBounding = false;*/
		maxDisp = 2;                      /*2 is more than 1, enough to trigger collider*/
	}
#ifdef YADE_OPENMP
	Real& thrMaxVSq = threadMaxVelocitySq[omp_get_thread_num()];
	thrMaxVSq       = max(thrMaxVSq, maxDisp);
#else
	maxVelocitySq = max(maxVelocitySq, maxDisp);
#endif
}

void NewtonIntegrator::action()
{
	timingDeltas->start();
	scene->forces.sync();
	timingDeltas->checkpoint("forces sync");
	bodySelected = (scene->selectedBody >= 0);
	if (warnNoForceReset && scene->forces.lastReset < scene->iter)
		LOG_WARN(
		        "O.forces last reset in step " << scene->forces.lastReset << ", while the current step is " << scene->iter
		                                       << ". Did you forget to include ForceResetter in O.engines?");
	const Real& dt = scene->dt;
	//Take care of user's request to change velGrad. Safe to change it here after the interaction loop.
	if (scene->cell->velGradChanged || scene->cell->nextVelGrad != Matrix3r::Zero()) {
		scene->cell->velGrad        = scene->cell->nextVelGrad;
		scene->cell->velGradChanged = 0;
		scene->cell->nextVelGrad    = Matrix3r::Zero();
	}
	homoDeform = scene->cell->homoDeform;
	dVelGrad   = scene->cell->velGrad - prevVelGrad;
	Matrix3r R = .5 * (dVelGrad - dVelGrad.transpose());
	dSpin      = Vector3r(-R(1, 2), R(0, 2), -R(0, 1));
	// account for motion of the periodic boundary, if we remember its last position
	// its velocity will count as max velocity of bodies
	// otherwise the collider might not run if only the cell were changing without any particle motion
	// FIXME: will not work for pure shear transformation, which does not change Cell::getSize()
	if (scene->isPeriodic && ((prevCellSize != scene->cell->getSize())) && /* initial value */ !math::isnan(prevCellSize[0])) {
		cellChanged   = true;
		maxVelocitySq = (prevCellSize - scene->cell->getSize()).squaredNorm() / pow(dt, 2);
	} else {
		maxVelocitySq = 0;
		cellChanged   = false;
	}

#ifdef YADE_BODY_CALLBACK
	// setup callbacks
	vector<BodyCallback::FuncPtr> callbackPtrs;
	for (const auto& cb : callbacks) {
		cerr << "<cb=" << cb.get() << ", setting cb->scene=" << scene << ">";
		cb->scene = scene;
		callbackPtrs.push_back(cb->stepInit());
	}
	assert(callbackPtrs.size() == callbacks.size());
	size_t callbacksSize = callbacks.size();
#endif

	const bool trackEnergy(scene->trackEnergy);
	const bool isPeriodic(scene->isPeriodic);

#ifdef YADE_OPENMP
	for (Real& thrMaxVSq : threadMaxVelocitySq) {
		thrMaxVSq = 0;
	}
#endif
	YADE_PARALLEL_FOREACH_BODY_BEGIN(const shared_ptr<Body>& b, scene->bodies)
	{
		// clump members are handled inside clumps
		if (b->isClumpMember()) continue;
		if ((mask > 0) and not b->maskCompatible(mask)) continue;
#ifdef YADE_MPI
		if (scene->subdomain != b->subdomain or (b->getIsSubdomain() or b->getIsFluidDomainBbox()))
			continue; //this thread will not move bodies from other subdomains
#endif
		State*            state = b->state.get();
		const Body::id_t& id    = b->getId();
		Vector3r          f     = Vector3r::Zero();
		Vector3r          m     = Vector3r::Zero();
		// clumps forces
		if (b->isClump()) {
			b->shape->cast<Clump>().addForceTorqueFromMembers(state, scene, f, m);
#ifdef YADE_OPENMP
			//it is safe here, since only one thread is adding forces/torques
			scene->forces.addTorqueUnsynced(id, m);
			scene->forces.addForceUnsynced(id, f);
#else
			scene->forces.addTorque(id, m);
			scene->forces.addForce(id, f);
#endif
		}
		//in most cases, the initial force on clumps will be zero and next line is not changing f and m, but make sure we don't miss something (e.g. user defined forces on clumps)
		f = scene->forces.getForce(id);
		m = scene->forces.getTorque(id);
#ifdef YADE_DEBUG
		if (math::isnan(f[0]) || math::isnan(f[1]) || math::isnan(f[2]))
			throw runtime_error(("NewtonIntegrator: NaN force acting on #" + boost::lexical_cast<string>(id) + ".").c_str());
		if (math::isnan(m[0]) || math::isnan(m[1]) || math::isnan(m[2]))
			throw runtime_error(("NewtonIntegrator: NaN torque acting on #" + boost::lexical_cast<string>(id) + ".").c_str());
		if (state->mass <= 0 && ((state->blockedDOFs & State::DOF_XYZ) != State::DOF_XYZ))
			throw runtime_error(
			        ("NewtonIntegrator: #" + boost::lexical_cast<string>(id)
			         + " has some linear accelerations enabled, but State::mass is non-positive."));
		if (state->inertia.minCoeff() <= 0 && ((state->blockedDOFs & State::DOF_RXRYRZ) != State::DOF_RXRYRZ))
			throw runtime_error(
			        ("NewtonIntegrator: #" + boost::lexical_cast<string>(id)
			         + " has some angular accelerations enabled, but State::inertia contains non-positive terms."));
#endif

		// fluctuation velocity does not contain meanfield velocity in periodic boundaries
		// in aperiodic boundaries, it is equal to absolute velocity
		Vector3r fluctVel = isPeriodic ? scene->cell->bodyFluctuationVel(b->state->pos, b->state->vel, prevVelGrad) : state->vel;

		// numerical damping & kinetic energy
		if (trackEnergy) updateEnergy(b, state, fluctVel, f, m);

		// whether to use aspherical rotation integration for this body; as soon as one axis of rotation is blocked the spherical integrator is "exact" (and faster),
		// we then switch to it. It also enables imposing clumps angVel directly (rather than momentum of the aspherical case)
		bool useAspherical = (exactAsphericalRot && b->isAspherical() && ((state->blockedDOFs & State::DOF_RXRYRZ) == State::DOF_NONE));

		// for particles not totally blocked, compute accelerations; otherwise, the computations would be useless
		if (state->blockedDOFs != State::DOF_ALL) {
			// linear acceleration
			Vector3r linAccel;
			if (dampGravity) {
				//original way, gravity is added before damping, so gravity is damped
				linAccel = computeAccel(f, state->mass, state->blockedDOFs);
				if (densityScaling) linAccel *= state->densityScaling;
				if (state->isDamped) cundallDamp2nd(dt, fluctVel, linAccel);
			} else {
				//new option, gravity is added after damping, so gravity is not damped
				linAccel = computeAccelWithoutGravity(f, state->mass, state->blockedDOFs);
				if (state->isDamped) cundallDamp2nd(dt, fluctVel, linAccel);
				linAccel += addGravity(state->blockedDOFs);
				if (densityScaling) linAccel *= state->densityScaling;
			}

			//This is the convective term, appearing in the time derivation of Cundall/Thornton expression (dx/dt=velGrad*pos -> d²x/dt²=dvelGrad/dt*pos+velGrad*vel), negligible in many cases but not for high speed large deformations (gaz or turbulent flow).
			if (isPeriodic && homoDeform > 1) linAccel += prevVelGrad * state->vel;
			//finally update velocity
			state->vel += dt * linAccel;
			// angular acceleration
			if (!useAspherical) { // uses angular velocity
				Vector3r angAccel = computeAngAccel(m, state->inertia, state->blockedDOFs);
				if (densityScaling) angAccel *= state->densityScaling;
				if (state->isDamped) cundallDamp2nd(dt, state->angVel, angAccel);
				state->angVel += dt * angAccel;
			} else { // uses torque
				for (int i = 0; i < 3; i++)
					if (state->blockedDOFs & State::axisDOF(i, true)) m[i] = 0; // block DOFs here
				if (state->isDamped) cundallDamp1st(m, state->angVel);
			}
			// reflect macro-deformation even for non-dynamic bodies
		} else if (isPeriodic && homoDeform > 1)
			state->vel += dt * prevVelGrad * state->vel;

		// update positions from velocities (or torque, for the aspherical integrator)
		leapfrogTranslate(state, dt);

		// Integrate the per-particle length scale lambda from the rate
		// lambdaDot supplied by the OpenFOAM coupling. Yade owns lambda:
		// OpenFOAM computes only the rate and reads the integrated value
		// back through FoamCoupling::sendBodyData(). Deliberately outside the
		// blockedDOFs test above -- a body loses lambda to chemistry even when
		// it is translationally fixed. lambdaDot is 0.0 unless FoamCoupling
		// set it, so this is a no-op in an uncoupled simulation. Integrating
		// here rather than in FoamCoupling::action() sub-cycles lambda at every
		// DEM step, while the coupling itself only runs every
		// dataExchangeInterval.
		state->lambda_ += state->lambdaDot * dt;
		if (!useAspherical) {
			leapfrogSphericalRotate(state, dt);
		} else {
			switch (rotAlgorithm) { // YADE_ENUM throws when trying to assign non existing enum value.
				case RotAlgorithm::delValle2023: leapfrogAsphericalRotateCarlos_2023(state, dt, m, scene->iter); break;
				case RotAlgorithm::Omelyan1998: leapfrogAsphericalRotateOmelyan_1998(state, dt, m, scene->iter); break;
				case RotAlgorithm::Fincham1992: leapfrogAsphericalRotate(state, dt, m); break;
				default:
					leapfrogAsphericalRotateCarlos_2023(state, dt, m, scene->iter);
					LOG_WARN("Unknown rotation algorithm: falling back to delValle2023's algorithm.");
					LOG_WARN("Available options are: delValle2023, Omelyan1998, Fincham1992.");
					break;
			}
		}

		saveMaximaDisplacement(b);
		// move individual members of the clump, save maxima velocity (for collider stride)
		if (b->isClump()) Clump::moveMembers(b, scene, this);

#ifdef YADE_BODY_CALLBACK
		// process callbacks
		for (size_t i = 0; i < callbacksSize; i++) {
			cerr << "<" << b->id << ",cb=" << callbacks[i] << ",scene=" << callbacks[i]->scene
			     << ">"; // <<",force="<<callbacks[i]->scene->forces.getForce(b->id)<<">";
			if (callbackPtrs[i] != NULL) (*(callbackPtrs[i]))(callbacks[i].get(), b.get());
		}
#endif
	}
	YADE_PARALLEL_FOREACH_BODY_END();
	timingDeltas->checkpoint("motion integration");
#ifdef YADE_OPENMP
	for (const Real& thrMaxVSq : threadMaxVelocitySq) {
		maxVelocitySq = max(maxVelocitySq, thrMaxVSq);
	}
#endif
	timingDeltas->checkpoint("sync max vel");
	if (scene->isPeriodic) {
		prevCellSize = scene->cell->getSize();
		prevVelGrad = scene->cell->prevVelGrad = scene->cell->velGrad;
	}
	timingDeltas->checkpoint("terminate");
}

void NewtonIntegrator::leapfrogTranslate(State* state, const Real& dt)
{
	// update velocity reflecting changes in the macroscopic velocity field, making the problem homothetic.
	//NOTE : if the velocity is updated before moving the body, it means the current velGrad (i.e. before integration in cell->integrateAndUpdate) will be effective for the current time-step. Is it correct? If not, this velocity update can be moved just after "state->pos += state->vel*dt", meaning the current velocity impulse will be applied at next iteration, after the contact law. (All this assuming the ordering is resetForces->integrateAndUpdate->contactLaw->PeriCompressor->NewtonsLaw. Any other might fool us.)
	//NOTE : dVel defined without wraping the coordinates means bodies out of the (0,0,0) period can move realy fast. It has to be compensated properly in the definition of relative velocities (see Ig2 functors and contact laws).
	//Reflect mean-field (periodic cell) acceleration in the velocity
	if (scene->isPeriodic && homoDeform) {
		Vector3r dVel = dVelGrad * state->pos;
		state->vel += dVel;
	}
	state->pos += state->vel * dt;
}

void NewtonIntegrator::leapfrogSphericalRotate(State* state, const Real& dt)
{
	if (scene->isPeriodic && homoDeform) { state->angVel += dSpin; }
	Real angle2 = state->angVel.squaredNorm();
	if (angle2 != 0) { //If we have an angular velocity, we make a rotation
		Real        angle = sqrt(angle2);
		Quaternionr q(AngleAxisr(angle * dt, state->angVel / angle));
		state->ori = q * state->ori;
	}
	state->ori.normalize();
}

void NewtonIntegrator::leapfrogAsphericalRotate(State* state, const Real& dt, const Vector3r& M)
{
	//FIXME: where to increment angular velocity like this? Only done for spherical rotations at the moment
	//if(scene->isPeriodic && homoDeform) {state->angVel+=dSpin;}
	Matrix3r       A          = state->ori.conjugate().toRotationMatrix(); // rotation matrix from global to local r.f.
	const Vector3r l_n        = state->angMom + dt / 2. * M;               // global angular momentum at time n
	const Vector3r l_b_n      = A * l_n;                                   // local angular momentum at time n
	Vector3r       angVel_b_n = l_b_n.cwiseQuotient(state->inertia);       // local angular velocity at time n
	if (densityScaling) angVel_b_n *= state->densityScaling;
	const Quaternionr dotQ_n = DotQ(angVel_b_n, state->ori);                                 // dQ/dt at time n
	const Quaternionr Q_half = Quaternionr(state->ori.coeffs() + dt / 2. * dotQ_n.coeffs()); // Q at time n+1/2
	state->angMom += dt * M;                                                                 // global angular momentum at time n+1/2
	const Vector3r l_b_half      = A * state->angMom;                                        // local angular momentum at time n+1/2
	Vector3r       angVel_b_half = l_b_half.cwiseQuotient(state->inertia);                   // local angular velocity at time n+1/2
	if (densityScaling) angVel_b_half *= state->densityScaling;
	const Quaternionr dotQ_half = DotQ(angVel_b_half, Q_half);                                // dQ/dt at time n+1/2
	state->ori                  = Quaternionr(state->ori.coeffs() + dt * dotQ_half.coeffs()); // Q at time n+1
	state->angVel               = state->ori * angVel_b_half;                                 // global angular velocity at time n+1/2
	state->ori.normalize();
}

void NewtonIntegrator::leapfrogAsphericalRotateOmelyan_1998(State* state, const Real& dt, const Vector3r& M, int iter)
{
	//FIXME: where to increment angular velocity like this? Only done for spherical rotations at the moment
	//if(scene->isPeriodic && homoDeform) {state->angVel+=dSpin;}
	Matrix3r A = state->ori.conjugate().toRotationMatrix(); // rotation matrix from global to local r.f.
	Vector3r w = A * state->angVel;                         // local angular velocity at time n
	if (densityScaling) w *= state->densityScaling;
	Vector3r       ww  = w;              // auxiliar vector
	const Vector3r II  = state->inertia; // auxiliar Inertia tensor vector
	const Vector3r tau = A * M;          // Torque in the local reference frame

	// Calculate angular velocity at time n + 1/2, solve nonlinear system of equations.
	for (int i = 0; i < niterOmelyan1998; i++)
		ww = w
		        + (tau
		           + 0.5
		                   * Vector3r(
		                           (w[1] * w[2] + ww[1] * ww[2]) * (II[1] - II[2]),
		                           (w[2] * w[0] + ww[2] * ww[0]) * (II[2] - II[0]),
		                           (w[0] * w[1] + ww[0] * ww[1]) * (II[0] - II[1])))
		                        .cwiseQuotient(II)
		                * dt;

	// Calculate quaternion at time n + 1
	const Real a = dt * dt * ww.squaredNorm() * 0.0625; // 1/16.0
	if (a != 0) {
		const Real        a1     = 1.0 - a;
		const Real        a2     = 1.0 + a;
		const Quaternionr dotQ_n = DotQ(ww, state->ori);                                                       // dQ/dt at time n + 1/2
		state->ori               = Quaternionr((a1 / a2) * state->ori.coeffs() + (dt / a2) * dotQ_n.coeffs()); // Q at time n+1
	}

	if (iter % normalizeEvery
	    == 0) // Just as a safety messure. In theory this is not needed. In reality it becomes unstable quite fast, dont use more than 300
		state->ori.normalize(); // This operation is expensive, we dont want to do it every time step.


	// Update angular velocity
	if (densityScaling) ww *= state->densityScaling;
	state->angMom = A.transpose() * ww.cwiseProduct(II); // global angular momentum at time n + 1/2
	state->angVel = A.transpose() * ww;                  // global angular velocity at time n + 1/2
}

void NewtonIntegrator::leapfrogAsphericalRotateCarlos_2023(State* state, const Real& dt, const Vector3r& M, int iter)
{
	//FIXME: where to increment angular velocity like this? Only done for spherical rotations at the moment
	//if(scene->isPeriodic && homoDeform) {state->angVel+=dSpin;}
	Matrix3r A = state->ori.conjugate().toRotationMatrix(); // rotation matrix from global to local r.f.
	Vector3r w = A * state->angVel;                         // local angular velocity at time n
	if (densityScaling) w *= state->densityScaling;
	const Vector3r tau = A * M; // Torque in the local reference frame

	// Calculate angular velocity at time n + 1/2, solve nonlinear system of equations.
	const Vector3r K1 = dt * w_dot(w, tau, state->inertia);
	const Vector3r K2 = dt * w_dot(w + K1, tau, state->inertia);
	const Vector3r K3 = dt * w_dot(w + 0.25 * (K1 + K2), tau, state->inertia);
	w += (K1 + K2 + 4.0 * K3) / 6.0;

	// Update orientation q(t + dt)
	Real w_Norm = w.squaredNorm();
	if (w_Norm != 0) { //If we have an angular velocity, we make a rotation
		w_Norm           = sqrt(w_Norm);
		const Real Theta = dt * w_Norm * 0.5;
		state->ori       = state->ori * Quaternionr(cos(Theta), sin(Theta) * w[0] / w_Norm, sin(Theta) * w[1] / w_Norm, sin(Theta) * w[2] / w_Norm);
	}

	if (iter % normalizeEvery
	    == 0) // Just as a safety messure. In theory this is not needed. The formulation preserves the norm. In my tests 10 k was fine, but just in case use 5k
		state->ori.normalize(); // This operation is expensive, we dont want to do it every time step.

	// Update angular velocity
	if (densityScaling) w *= state->densityScaling;
	state->angMom = A.transpose() * w.cwiseProduct(state->inertia); // global angular momentum at time n + 1/2
	state->angVel = A.transpose() * w;                              // global angular velocity at time n + 1/2
}

Vector3r NewtonIntegrator::w_dot(const Vector3r w, const Vector3r M, const Vector3r II)
{
	return Vector3r(
	        (M[0] + w[1] * w[2] * (II[1] - II[2])) / II[0], (M[1] + w[2] * w[0] * (II[2] - II[0])) / II[1], (M[2] + w[0] * w[1] * (II[0] - II[1])) / II[2]);
}

bool NewtonIntegrator::get_densityScaling() const
{
	for (const auto& e : Omega::instance().getScene()->engines) {
		GlobalStiffnessTimeStepper* ts = dynamic_cast<GlobalStiffnessTimeStepper*>(e.get());
		if (ts && densityScaling != ts->densityScaling)
			LOG_WARN("density scaling is not active in the timeStepper, it will have no effect unless a scaling is specified manually for some "
			         "bodies");
	}
	LOG_WARN("GlobalStiffnessTimeStepper not present in O.engines, density scaling will have no effect unless a scaling is specified manually for some "
	         "bodies");
	return densityScaling;
	;
}

void NewtonIntegrator::set_densityScaling(bool dsc)
{
	for (const auto& e : Omega::instance().getScene()->engines) {
		GlobalStiffnessTimeStepper* ts = dynamic_cast<GlobalStiffnessTimeStepper*>(e.get());
		if (ts) {
			ts->densityScaling = dsc;
			densityScaling     = dsc;
			LOG_WARN("GlobalStiffnessTimeStepper found in O.engines and adjusted to match this setting. Revert in the timestepper if you don't "
			         "want the scaling adjusted automatically.");
			return;
		}
	}
	LOG_WARN("GlobalStiffnessTimeStepper not found in O.engines. Density scaling will have no effect unless a scaling is specified manually for some "
	         "bodies");
}


// http://www.euclideanspace.com/physics/kinematics/angularvelocity/QuaternionDifferentiation2.pdf
Quaternionr NewtonIntegrator::DotQ(const Vector3r& angVel, const Quaternionr& Q)
{
	Quaternionr dotQ;
	dotQ.w() = (-Q.x() * angVel[0] - Q.y() * angVel[1] - Q.z() * angVel[2]) / 2;
	dotQ.x() = (Q.w() * angVel[0] - Q.z() * angVel[1] + Q.y() * angVel[2]) / 2;
	dotQ.y() = (Q.z() * angVel[0] + Q.w() * angVel[1] - Q.x() * angVel[2]) / 2;
	dotQ.z() = (-Q.y() * angVel[0] + Q.x() * angVel[1] + Q.w() * angVel[2]) / 2;
	return dotQ;
}

} // namespace yade
