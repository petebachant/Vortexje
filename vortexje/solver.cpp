//
// Vortexje -- Solver.
//
// Copyright (C) 2012 - 2014 Baayen & Heinz GmbH.
//
// Authors: Jorn Baayen <jorn.baayen@baayen-heinz.com>
//

#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>

#include <iostream>
#include <limits>
#include <typeinfo>

#ifdef _WIN32
#include <direct.h>
#endif

#include <Eigen/Geometry>
#include <Eigen/IterativeLinearSolvers>

#include <vortexje/solver.hpp>
#include <vortexje/parameters.hpp>
#include <vortexje/boundary-layers/dummy-boundary-layer.hpp>

using namespace std;
using namespace Eigen;
using namespace Vortexje;

// Helper to create folders:
static void
mkdir_helper(const string folder)
{
#ifdef _WIN32
    if (mkdir(folder.c_str()) < 0)
#else
    if (mkdir(folder.c_str(), S_IRWXU) < 0)
#endif
        if (errno != EEXIST)
            cerr << "Could not create log folder " << folder << ": " << strerror(errno) << endl;
}

/**
   Construct a solver, logging its output into the given folder.
   
   @param[in]   log_folder  Logging output folder.
*/
Solver::Solver(const string &log_folder) : log_folder(log_folder)
{ 
    // Initialize wind:
    freestream_velocity = Vector3d(0, 0, 0);
    
    // Initialize fluid density:
    fluid_density = 0.0;
    
    // Total number of panels:
    n_non_wake_panels = 0;
        
    // Open log files:
    mkdir_helper(log_folder);
}

/**
   Destructor.
*/
Solver::~Solver()
{
}

/**
   Adds a surface body to this solver.
   
   @param[in]   body   Body to be added.
*/
void
Solver::add_body(Body &body)
{
    // TODO Eventually move to std::reference_wrapper, once we switch to C++11.
    bodies.push_back(&body);
    
    vector<Body::SurfaceData*>::iterator si;
    for (si = body.non_lifting_surfaces.begin(); si != body.non_lifting_surfaces.end(); si++) {
        Body::SurfaceData *d = *si;
        
        non_wake_surfaces.push_back(d);
           
        surface_id_to_body[d->surface.id] = &body;
        
        n_non_wake_panels += d->surface.n_panels();
    }
    
    vector<Body::LiftingSurfaceData*>::iterator lsi;
    for (lsi = body.lifting_surfaces.begin(); lsi != body.lifting_surfaces.end(); lsi++) {
        Body::LiftingSurfaceData *d = *lsi;
        
        non_wake_surfaces.push_back(d);
           
        surface_id_to_body[d->surface.id] = &body;
        surface_id_to_body[d->wake.id] = &body;
        
        n_non_wake_panels += d->lifting_surface.n_panels();
    }
    
    doublet_coefficients.resize(n_non_wake_panels);
    doublet_coefficients.setZero();
    
    source_coefficients.resize(n_non_wake_panels);
    source_coefficients.setZero();
    
    surface_velocity_potentials.resize(n_non_wake_panels);
    surface_velocity_potentials.setZero();
    
    surface_velocities.resize(n_non_wake_panels, 3);
    surface_velocities.setZero();
    
    pressure_coefficients.resize(n_non_wake_panels);
    pressure_coefficients.setZero();
    
    previous_surface_velocity_potentials.resize(n_non_wake_panels);
    previous_surface_velocity_potentials.setZero();

    // Open logs:
    string body_log_folder = log_folder + "/" + body.id;
    
    mkdir_helper(body_log_folder);
    
    for (int i = 0; i < (int) body.non_lifting_surfaces.size(); i++) {
        stringstream ss;
        ss << body_log_folder << "/non_lifting_surface_" << i;
        
        string s = ss.str();
        mkdir_helper(s);
    }
    
    for (int i = 0; i < (int) body.lifting_surfaces.size(); i++) {
        stringstream ss;
        ss << body_log_folder << "/lifting_surface_" << i;
        
        string s = ss.str();
        mkdir_helper(s);
        
        ss.str(string());
        ss.clear();
        ss << body_log_folder << "/wake_" << i;
        
        s = ss.str();
        mkdir_helper(s);      
    }
}

/**
   Sets the freestream velocity.
   
   @param[in]   value   Freestream velocity.
*/
void
Solver::set_freestream_velocity(const Vector3d &value)
{
    freestream_velocity = value;
}

/**
   Sets the fluid density.
   
   @param[in]   value   Fluid density.
*/
void
Solver::set_fluid_density(double value)
{
    fluid_density = value;
}

/**
   Computes the velocity potential at the given point.
   
   @param[in]   x   Reference point.
   
   @returns Velocity potential.
*/
double
Solver::velocity_potential(const Vector3d &x) const
{
    // Sum disturbance potential with freestream velocity potential:
    return compute_disturbance_velocity_potential(x) + freestream_velocity.dot(x);
}

/**
   Computes the total stream velocity at the given point.
   
   @param[in]   x   Reference point.
   
   @returns Stream velocity.
*/
Eigen::Vector3d
Solver::velocity(const Eigen::Vector3d &x) const
{
    // Sum disturbance velocity with freestream velocity:
    return compute_disturbance_velocity(x) + freestream_velocity;
}

/**
   Returns the surface velocity potential for the given panel.
   
   @param[in]   surface   Reference surface.
   @param[in]   panel     Reference panel.
  
   @returns Surface velocity potential.
*/
double
Solver::surface_velocity_potential(const Surface &surface, int panel) const
{
    int offset = 0;
    
    vector<Body::SurfaceData*>::const_iterator si;
    for (si = non_wake_surfaces.begin(); si != non_wake_surfaces.end(); si++) {
        const Body::SurfaceData *d = *si;
        
        if (&surface == &d->surface)
            return surface_velocity_potentials(offset + panel);
        
        offset += d->surface.n_panels();
    }
    
    cerr << "Solver::surface_velocity_potential():  Panel " << panel << " not found on surface " << surface.id << "." << endl;
    
    return 0.0;
}

/**
   Returns the surface velocity for the given panel.
   
   @param[in]   surface   Reference surface.
   @param[in]   panel     Reference panel.
  
   @returns Surface velocity.
*/
Vector3d
Solver::surface_velocity(const Surface &surface, int panel) const
{
    int offset = 0;
    
    vector<Body::SurfaceData*>::const_iterator si;
    for (si = non_wake_surfaces.begin(); si != non_wake_surfaces.end(); si++) {
        const Body::SurfaceData *d = *si;
        
        if (&surface == &d->surface)
            return surface_velocities.block<1, 3>(offset + panel, 0);
        
        offset += d->surface.n_panels();
    }
    
    cerr << "Solver::surface_velocity():  Panel " << panel << " not found on surface " << surface.id << "." << endl;
    
    return Vector3d(0, 0, 0);
}

/**
   Returns the pressure coefficient for the given panel.
   
   @param[in]   surface   Reference saceurf.
   @param[in]   panel     Reference panel.
  
   @returns Pressure coefficient.
*/
double
Solver::pressure_coefficient(const Surface &surface, int panel) const
{
    int offset = 0;
    
    vector<Body::SurfaceData*>::const_iterator si;
    for (si = non_wake_surfaces.begin(); si != non_wake_surfaces.end(); si++) {
        const Body::SurfaceData *d = *si;
        
        if (&surface == &d->surface)
            return pressure_coefficients(offset + panel);
        
        offset += d->surface.n_panels();
    }
    
    cerr << "Solver::pressure_coefficient():  Panel " << panel << " not found on surface " << surface.id << "." << endl;
    
    return 0.0;
}

/**
   Computes the force caused by the pressure distribution on the given body.
   
   @param[in]   body   Reference body.
  
   @returns Force vector.
*/
Eigen::Vector3d
Solver::force(const Body &body) const
{
    // Dynamic pressure:
    double q = 0.5 * fluid_density * compute_reference_velocity_squared(body);
        
    // Total force on body:
    Vector3d F(0, 0, 0);
    int offset = 0;
    
    vector<Body::SurfaceData*>::const_iterator si;
    for (si = non_wake_surfaces.begin(); si != non_wake_surfaces.end(); si++) {
        const Body::SurfaceData *d = *si;
        
        Body *si_body = surface_id_to_body.find(d->surface.id)->second;
        if (&body == si_body) {        
            for (int i = 0; i < d->surface.n_panels(); i++) {
                const Vector3d &normal = d->surface.panel_normal(i);
                double surface_area = d->surface.panel_surface_area(i);
                F += q * surface_area * pressure_coefficients(offset + i) * normal;
                
                F += d->boundary_layer.friction(i);
            }
        }
        
        offset += d->surface.n_panels();
    }
                    
    // Done:
    return F;      
}

/**
   Computes the moment caused by the pressure distribution on the given body, relative to the given point.
   
   @param[in]   body   Reference body.
   @param[in]   x      Reference point.
  
   @returns Moment vector.
*/
Eigen::Vector3d
Solver::moment(const Body &body, const Eigen::Vector3d &x) const
{
    // Dynamic pressure:
    double q = 0.5 * fluid_density * compute_reference_velocity_squared(body);
        
    // Total moment on body:
    Vector3d M(0, 0, 0);
    int offset = 0;
    
    vector<Body::SurfaceData*>::const_iterator si;
    for (si = non_wake_surfaces.begin(); si != non_wake_surfaces.end(); si++) {
        const Body::SurfaceData *d = *si;
        
        Body *si_body = surface_id_to_body.find(d->surface.id)->second;
        if (&body == si_body) { 
            for (int i = 0; i < d->surface.n_panels(); i++) {                                    
                const Vector3d &normal = d->surface.panel_normal(i);
                double surface_area = d->surface.panel_surface_area(i);
                Vector3d F = q * surface_area * pressure_coefficients(offset + i) * normal;
                
                F += d->boundary_layer.friction(i);
                    
                Vector3d r = d->surface.panel_collocation_point(i, false) - x;
                M += r.cross(F);
            }
        }
        
        offset += d->surface.n_panels();
    }
    
    // Done:
    return M;
}

/**
   Initializes the wakes by adding a first layer of vortex ring panels.
   
   @param[in]   dt   Time step size.
*/
void
Solver::initialize_wakes(double dt)
{
    // Add initial wake layers:
    vector<Body*>::iterator bi;
    for (bi = bodies.begin(); bi != bodies.end(); bi++) {
        Body *body = *bi;
    
        vector<Body::LiftingSurfaceData*>::iterator lsi;
        for (lsi = body->lifting_surfaces.begin(); lsi != body->lifting_surfaces.end(); lsi++) {
            Body::LiftingSurfaceData *d = *lsi;
            
            d->wake.add_layer();
            for (int i = 0; i < d->lifting_surface.n_spanwise_nodes(); i++) {
                if (Parameters::convect_wake) {
                    // Convect wake nodes that coincide with the trailing edge.
                    d->wake.nodes[i] += compute_trailing_edge_vortex_displacement(*body, d->lifting_surface, i, dt);
                    
                } else {
                    // Initialize static wake.
                    Vector3d body_apparent_velocity = body->velocity - freestream_velocity;
                    
                    d->wake.nodes[i] -= Parameters::static_wake_length * body_apparent_velocity / body_apparent_velocity.norm();
                }
            }
            
            d->wake.add_layer();
        }
    }
}

/**
   Computes new source, doublet, and pressure distributions.
   
   @param[in]   dt          Time step size.
   @param[in]   propagate   Propagate solution forward in time.
   
   @returns true on success.
*/
bool
Solver::solve(double dt, bool propagate)
{
    int offset;
    
    // Iterate inviscid and boundary layer solutions until convergence.
    int boundary_layer_iteration = 0;
    while (true) {
        // Compute source distribution:
        cout << "Solver: Computing source distribution with wake influence." << endl;
        
        offset = 0;
        
        vector<Body::SurfaceData*>::iterator si;
        for (si = non_wake_surfaces.begin(); si != non_wake_surfaces.end(); si++) {
            Body::SurfaceData *d = *si;
            int i;
            
            Body *body = surface_id_to_body.find(d->surface.id)->second;
            
            #pragma omp parallel
            {
                #pragma omp for schedule(dynamic, 1)
                for (i = 0; i < d->surface.n_panels(); i++)
                    source_coefficients(offset + i) = compute_source_coefficient(*body, d->surface, i, d->boundary_layer, true);
            }
            
            offset += d->surface.n_panels();
        }
      
        // Populate the matrices of influence coefficients:
        cout << "Solver: Computing matrices of influence coefficients." << endl;
        
        MatrixXd A(n_non_wake_panels, n_non_wake_panels);
        MatrixXd source_influence_coefficients(n_non_wake_panels, n_non_wake_panels);
        
        int offset_row = 0, offset_col = 0;
        
        vector<Body::SurfaceData*>::iterator si_row;
        for (si_row = non_wake_surfaces.begin(); si_row != non_wake_surfaces.end(); si_row++) {
            Body::SurfaceData *d_row = *si_row;

            offset_col = 0;
     
            // Influence coefficients between all non-wake surfaces:
            vector<Body::SurfaceData*>::iterator si_col;
            for (si_col = non_wake_surfaces.begin(); si_col != non_wake_surfaces.end(); si_col++) {
                Body::SurfaceData *d_col = *si_col;
                int i, j;
                
                #pragma omp parallel private(j) 
                {
                    #pragma omp for schedule(dynamic, 1)
                    for (i = 0; i < d_row->surface.n_panels(); i++) {
                        for (j = 0; j < d_col->surface.n_panels(); j++) {
                            d_col->surface.source_and_doublet_influence(d_row->surface, i, j,
                                                                        source_influence_coefficients(offset_row + i, offset_col + j), 
                                                                        A(offset_row + i, offset_col + j));
                        }
                    }
                }
                
                offset_col = offset_col + d_col->surface.n_panels();
            }
            
            // The influence of the new wake panels:
            int i, j, lifting_surface_offset, wake_panel_offset, pa, pb;
            vector<Body*>::const_iterator bi;
            vector<Body::SurfaceData*>::const_iterator si;
            vector<Body::LiftingSurfaceData*>::const_iterator lsi;
            const Body *body;
            const Body::LiftingSurfaceData *d;
            
            #pragma omp parallel private(bi, si, lsi, lifting_surface_offset, j, wake_panel_offset, pa, pb, body, d)
            {
                #pragma omp for schedule(dynamic, 1)
                for (i = 0; i < d_row->surface.n_panels(); i++) {
                    lifting_surface_offset = 0;      
                    
                    for (bi = bodies.begin(); bi != bodies.end(); bi++) {
                        body = *bi;
                        
                        for (si = body->non_lifting_surfaces.begin(); si != body->non_lifting_surfaces.end(); si++)
                            lifting_surface_offset += (*si)->surface.n_panels();
                                      
                        for (lsi = body->lifting_surfaces.begin(); lsi != body->lifting_surfaces.end(); lsi++) {
                            d = *lsi;
                            
                            wake_panel_offset = d->wake.n_panels() - d->lifting_surface.n_spanwise_panels();
                            for (j = 0; j < d->lifting_surface.n_spanwise_panels(); j++) {  
                                pa = d->lifting_surface.trailing_edge_upper_panel(j);
                                pb = d->lifting_surface.trailing_edge_lower_panel(j);
                                
                                // Account for the influence of the new wake panels.  The doublet strength of these panels
                                // is set according to the Kutta condition.
                                A(offset_row + i, lifting_surface_offset + pa) += d->wake.doublet_influence(d_row->surface, i, wake_panel_offset + j);
                                A(offset_row + i, lifting_surface_offset + pb) -= d->wake.doublet_influence(d_row->surface, i, wake_panel_offset + j);
                            }
                            
                            lifting_surface_offset += d->lifting_surface.n_panels();
                        }
                    }
                }
            }
                
            offset_row = offset_row + d_row->surface.n_panels();
        }
        
        // Compute the doublet distribution:
        cout << "Solver: Computing doublet distribution." << endl;
        
        VectorXd b = source_influence_coefficients * source_coefficients;
        
        BiCGSTAB<MatrixXd> solver(A);
        solver.setMaxIterations(Parameters::linear_solver_max_iterations);
        solver.setTolerance(Parameters::linear_solver_tolerance);

        VectorXd new_doublet_coefficients = solver.solveWithGuess(b, doublet_coefficients);
        
        if (solver.info() != Success) {
            cerr << "Solver: Computing doublet distribution failed (" << solver.iterations();
            cerr << " iterations with estimated error=" << solver.error() << ")." << endl;
           
            return false;
        }
        
        cout << "Solver: Done computing doublet distribution in " << solver.iterations() << " iterations with estimated error " << solver.error() << "." << endl;

        // Check for convergence from second iteration onwards.
        // (On the first iteration, the value of doublet_coefficients originates from the previous call to solve().
        bool converged = false;
        if (boundary_layer_iteration > 0)
            if ((new_doublet_coefficients - doublet_coefficients).norm() < Parameters::boundary_layer_iteration_tolerance)
                converged = true;
        
        doublet_coefficients = new_doublet_coefficients;
        
        // Set new wake panel doublet coefficients:
        cout << "Solver: Updating wake doublet distribution." << endl;
        
        offset = 0;
        
        vector<Body*>::iterator bi;
        for (bi = bodies.begin(); bi != bodies.end(); bi++) {
            Body *body = *bi;
            
            vector<Body::SurfaceData*>::iterator si;
            for (si = body->non_lifting_surfaces.begin(); si != body->non_lifting_surfaces.end(); si++)
                offset += (*si)->surface.n_panels();
            
            vector<Body::LiftingSurfaceData*>::iterator lsi;
            for (lsi = body->lifting_surfaces.begin(); lsi != body->lifting_surfaces.end(); lsi++) {
                Body::LiftingSurfaceData *d = *lsi;
                         
                // Set panel doublet coefficient:
                for (int i = 0; i < d->lifting_surface.n_spanwise_panels(); i++) {
                    double doublet_coefficient_top    = doublet_coefficients(offset + d->lifting_surface.trailing_edge_upper_panel(i));
                    double doublet_coefficient_bottom = doublet_coefficients(offset + d->lifting_surface.trailing_edge_lower_panel(i));
                    
                    // Use the trailing-edge Kutta condition to compute the doublet coefficients of the new wake panels.
                    double doublet_coefficient = doublet_coefficient_top - doublet_coefficient_bottom;
                    
                    int idx = d->wake.n_panels() - d->lifting_surface.n_spanwise_panels() + i;
                    d->wake.doublet_coefficients[idx] = doublet_coefficient;
                }
                
                // Update offset:
                offset += d->lifting_surface.n_panels();
            }
        }
        
        // Compute surface velocity distribution:
        cout << "Solver: Computing surface velocity distribution." << endl;
        
        offset = 0;

        for (si = non_wake_surfaces.begin(); si != non_wake_surfaces.end(); si++) {
            Body::SurfaceData *d = *si;
            int i;
                
            #pragma omp parallel
            {
                #pragma omp for schedule(dynamic, 1)
                for (i = 0; i < d->surface.n_panels(); i++)
                    surface_velocities.block<1, 3>(offset + i, 0) = compute_surface_velocity(d->surface, offset, i);
            }
            
            offset += d->surface.n_panels();      
        } 
        
        // If we converged, then this is the time to break out of the loop.
        if (converged) {
            cout << "Solver: Boundary layer iteration converged." << endl;
            
            break;
        }
        
        if (boundary_layer_iteration > Parameters::max_boundary_layer_iterations) {
            cout << "Solver: Maximum number of boundary layer iterations ranged.  Aborting iteration." << endl;
            
            break;
        }
        
        // Recompute the boundary layers.
        bool have_boundary_layer = false;
        
        offset = 0;
        
        for (si = non_wake_surfaces.begin(); si != non_wake_surfaces.end(); si++) {
            Body::SurfaceData *d = *si;
            
            if (typeid(d->boundary_layer) != typeid(DummyBoundaryLayer)) {
                have_boundary_layer = true;
                
                d->boundary_layer.recalculate(surface_velocities.block(offset, 0, d->surface.n_panels(), 3));
            }
                
            offset += d->surface.n_panels();
        }
        
        // Did we did not find any boundary layers, then there is no need to iterate.
        if (!have_boundary_layer)
            break;
        
        // Increase iteration counter:
        boundary_layer_iteration++;
    }

    if (Parameters::convect_wake) {
        // Recompute source distribution without wake influence:
        cout << "Solver: Recomputing source distribution without wake influence." << endl;
        
        offset = 0;
        
        vector<Body::SurfaceData*>::iterator si;
        for (si = non_wake_surfaces.begin(); si != non_wake_surfaces.end(); si++) {
            Body::SurfaceData *d = *si;
            int i;
            
            Body *body = surface_id_to_body.find(d->surface.id)->second;
            
            #pragma omp parallel
            {
                #pragma omp for schedule(dynamic, 1)
                for (i = 0; i < d->surface.n_panels(); i++)
                    source_coefficients(offset + i) = compute_source_coefficient(*body, d->surface, i, d->boundary_layer, false);
            }
            
            offset += d->surface.n_panels();
        }
    }

    // Compute pressure distribution:
    cout << "Solver: Computing pressure distribution." << endl;
    
    offset = 0;

    vector<Body::SurfaceData*>::iterator si;   
    for (si = non_wake_surfaces.begin(); si != non_wake_surfaces.end(); si++) {
        Body::SurfaceData *d = *si;
        int i;
        
        Body *body = surface_id_to_body.find(d->surface.id)->second;
        double v_ref_squared = compute_reference_velocity_squared(*body);
        
        double dphidt;
            
        #pragma omp parallel private(dphidt)
        {
            #pragma omp for schedule(dynamic, 1)
            for (i = 0; i < d->surface.n_panels(); i++) {
                // Velocity potential:
                surface_velocity_potentials(offset + i) = compute_surface_velocity_potential(d->surface, offset, i);
                
                // Pressure coefficient:
                dphidt = compute_surface_velocity_potential_time_derivative(offset, i, dt);
                pressure_coefficients(offset + i) = compute_pressure_coefficient(surface_velocities.block<1, 3>(offset + i, 0), dphidt, v_ref_squared);
            }
        }
        
        offset += d->surface.n_panels();      
    }
    
    // Propagate solution forward in time, if requested.
    if (propagate)
        this->propagate();
    
    // Done:
    return true;
}

/**
   Propagates solution forward in time.  Relevant in unsteady mode only.
*/
void
Solver::propagate()
{
    // Store previous values of the surface velocity potentials:
    previous_surface_velocity_potentials = surface_velocity_potentials;
}

/**
   Convects existing wake nodes, and emits a new layer of wake panels.
   
   @param[in]   dt   Time step size.
*/
void
Solver::update_wakes(double dt)
{
    // Do we convect wake panels?
    if (Parameters::convect_wake) {
        cout << "Solver: Convecting wakes." << endl;
        
        // Compute velocity values at wake nodes, with the wakes in their original state:
        std::vector<std::vector<Vector3d> > wake_velocities;
        
        vector<Body*>::iterator bi;
        for (bi = bodies.begin(); bi != bodies.end(); bi++) {
            Body *body = *bi;
                 
            vector<Body::LiftingSurfaceData*>::iterator lsi;
            for (lsi = body->lifting_surfaces.begin(); lsi != body->lifting_surfaces.end(); lsi++) {
                Body::LiftingSurfaceData *d = *lsi;
                
                std::vector<Vector3d> local_wake_velocities;
                local_wake_velocities.resize(d->wake.n_nodes());
                
                int i;
                
                #pragma omp parallel
                {
                    #pragma omp for schedule(dynamic, 1)
                    for (i = 0; i < d->wake.n_nodes(); i++)
                        local_wake_velocities[i] = velocity(d->wake.nodes[i]);
                }
                
                wake_velocities.push_back(local_wake_velocities);
            }
        }
        
        // Add new wake panels at trailing edges, and convect all vertices:
        int idx = 0;
        
        for (bi = bodies.begin(); bi != bodies.end(); bi++) {
            Body *body = *bi;
            
            vector<Body::LiftingSurfaceData*>::iterator lsi;
            for (lsi = body->lifting_surfaces.begin(); lsi != body->lifting_surfaces.end(); lsi++) {
                Body::LiftingSurfaceData *d = *lsi;
                
                // Retrieve local wake velocities:
                std::vector<Vector3d> &local_wake_velocities = wake_velocities[idx];
                idx++;
                
                // Convect wake nodes that coincide with the trailing edge.
                for (int i = 0; i < d->lifting_surface.n_spanwise_nodes(); i++) {                                                  
                    d->wake.nodes[d->wake.n_nodes() - d->lifting_surface.n_spanwise_nodes() + i]
                        += compute_trailing_edge_vortex_displacement(*body, d->lifting_surface, i, dt);
                }                
                
                // Convect all other wake nodes according to the local wake velocity:
                int i;
                
                #pragma omp parallel
                {
                    #pragma omp for schedule(dynamic, 1)
                    for (i = 0; i < d->wake.n_nodes() - d->lifting_surface.n_spanwise_nodes(); i++)
                        d->wake.nodes[i] += local_wake_velocities[i] * dt;
                }
                    
                // Run internal wake update:
                d->wake.update_properties(dt);

                // Add new vertices:
                // (This call also updates the geometry)
                d->wake.add_layer();
            }
        }
        
    } else {
        cout << "Solver: Re-positioning wakes." << endl;
        
        // No wake convection.  Re-position wake:
        vector<Body*>::iterator bi;
        for (bi = bodies.begin(); bi != bodies.end(); bi++) {
            Body *body = *bi;
            
            Vector3d body_apparent_velocity = body->velocity - freestream_velocity;
            
            vector<Body::LiftingSurfaceData*>::iterator lsi;
            for (lsi = body->lifting_surfaces.begin(); lsi != body->lifting_surfaces.end(); lsi++) {
                Body::LiftingSurfaceData *d = *lsi;
            
                for (int i = 0; i < d->lifting_surface.n_spanwise_nodes(); i++) {
                    // Connect wake to trailing edge nodes:                             
                    d->wake.nodes[d->lifting_surface.n_spanwise_nodes() + i] = d->lifting_surface.nodes[d->lifting_surface.trailing_edge_node(i)];
                    
                    // Point wake in direction of body kinematic velocity:
                    d->wake.nodes[i] = d->lifting_surface.nodes[d->lifting_surface.trailing_edge_node(i)]
                                     - Parameters::static_wake_length * body_apparent_velocity / body_apparent_velocity.norm();
                }
                
                // Need to update geometry:
                d->wake.compute_geometry();
            }
        }
    }
}

/**
   Logs source and doublet distributions, as well as the pressure coefficients, into files in the logging folder
   tagged with the specified step number.
   
   @param[in]   step_number   Step number used to name the output files.
   @param[in]   writer        SurfaceWriter object to use.
*/
void
Solver::log(int step_number, SurfaceWriter &writer) const
{   
    // Log coefficients: 
    int offset = 0;
    int save_node_offset = 0;
    int save_panel_offset = 0;
    int idx;
    
    vector<Body*>::const_iterator bi;
    for (bi = bodies.begin(); bi != bodies.end(); bi++) {
        const Body *body = *bi;
        
        // Iterate non-lifting surfaces:
        idx = 0;
        
        vector<Body::SurfaceData*>::const_iterator si;
        for (si = body->non_lifting_surfaces.begin(); si != body->non_lifting_surfaces.end(); si++) {
            const Body::SurfaceData *d = *si;
            
            // Log non-lifting surface coefficients:
            VectorXd non_lifting_surface_doublet_coefficients(d->surface.n_panels());
            VectorXd non_lifting_surface_source_coefficients(d->surface.n_panels());
            VectorXd non_lifting_surface_pressure_coefficients(d->surface.n_panels());
            for (int i = 0; i < d->surface.n_panels(); i++) {
                non_lifting_surface_doublet_coefficients(i)  = doublet_coefficients(offset + i);
                non_lifting_surface_source_coefficients(i)   = source_coefficients(offset + i);
                non_lifting_surface_pressure_coefficients(i) = pressure_coefficients(offset + i);
            }
            
            offset += d->surface.n_panels();
            
            vector<string> view_names;
            vector<VectorXd> view_data;
            
            view_names.push_back("DoubletDistribution");
            view_data.push_back(non_lifting_surface_doublet_coefficients);
            
            view_names.push_back("SourceDistribution");
            view_data.push_back(non_lifting_surface_source_coefficients);
            
            view_names.push_back("PressureDistribution");
            view_data.push_back(non_lifting_surface_pressure_coefficients);
            
            std::stringstream ss;
            ss << log_folder << "/" << body->id << "/non_lifting_surface_" << idx << "/step_" << step_number << writer.file_extension();

            writer.write(d->surface, ss.str(), save_node_offset, save_panel_offset, view_names, view_data);
            
            save_node_offset += d->surface.n_nodes();
            save_panel_offset += d->surface.n_panels();
            
            idx++;
        }   
        
        // Iterate lifting surfaces:
        idx = 0;
        
        vector<Body::LiftingSurfaceData*>::const_iterator lsi;
        for (lsi = body->lifting_surfaces.begin(); lsi != body->lifting_surfaces.end(); lsi++) {
            const Body::LiftingSurfaceData *d = *lsi;
            
            // Log lifting surface coefficients:
            VectorXd lifting_surface_doublet_coefficients(d->lifting_surface.n_panels());
            VectorXd lifting_surface_source_coefficients(d->lifting_surface.n_panels());
            VectorXd lifting_surface_pressure_coefficients(d->lifting_surface.n_panels());
            for (int i = 0; i < d->lifting_surface.n_panels(); i++) {
                lifting_surface_doublet_coefficients(i)  = doublet_coefficients(offset + i);
                lifting_surface_source_coefficients(i)   = source_coefficients(offset + i);
                lifting_surface_pressure_coefficients(i) = pressure_coefficients(offset + i);
            }
            
            offset += d->lifting_surface.n_panels();
            
            vector<string> view_names;
            vector<VectorXd> view_data;
            
            view_names.push_back("DoubletDistribution");
            view_data.push_back(lifting_surface_doublet_coefficients);
            
            view_names.push_back("SourceDistribution");
            view_data.push_back(lifting_surface_source_coefficients);
            
            view_names.push_back("PressureDistribution");
            view_data.push_back(lifting_surface_pressure_coefficients);
            
            std::stringstream ss;
            ss << log_folder << "/" << body->id << "/lifting_surface_" << idx << "/step_" << step_number << writer.file_extension();

            writer.write(d->lifting_surface, ss.str(), save_node_offset, save_panel_offset, view_names, view_data);
            
            save_node_offset += d->lifting_surface.n_nodes();
            save_panel_offset += d->lifting_surface.n_panels();
    
            // Log wake surface and coefficients:
            VectorXd wake_doublet_coefficients(d->wake.doublet_coefficients.size());
            for (int i = 0; i < (int) d->wake.doublet_coefficients.size(); i++)
                wake_doublet_coefficients(i) = d->wake.doublet_coefficients[i];

            view_names.clear();
            view_data.clear();
            
            view_names.push_back("DoubletDistribution");
            view_data.push_back(wake_doublet_coefficients);
            
            std::stringstream ssw;
            ssw << log_folder << "/" << body->id << "/wake_" << idx << "/step_" << step_number << writer.file_extension();
            
            writer.write(d->wake, ssw.str(), 0, save_panel_offset, view_names, view_data);
            
            save_node_offset += d->wake.n_nodes();
            save_panel_offset += d->wake.n_panels();
            
            idx++;
        }
    }
}
 
// Compute source coefficient for given surface and panel:
double
Solver::compute_source_coefficient(const Body &body, const Surface &surface, int panel, const BoundaryLayer &boundary_layer, bool include_wake_influence) const
{
    // Start with apparent velocity:
    Vector3d velocity = body.panel_kinematic_velocity(surface, panel) - freestream_velocity;
    
    // Wake contribution:
    if (Parameters::convect_wake && include_wake_influence) {
        vector<Body*>::const_iterator bi;
        for (bi = bodies.begin(); bi != bodies.end(); bi++) {
            const Body *body = *bi;
            
            vector<Body::LiftingSurfaceData*>::const_iterator lsi;
            for (lsi = body->lifting_surfaces.begin(); lsi != body->lifting_surfaces.end(); lsi++) {
                const Body::LiftingSurfaceData *d = *lsi;
                
                // Add influence of old wake panels.  That is, those wake panels which already have a doublet
                // strength assigned to them.
                for (int k = 0; k < d->wake.n_panels() - d->lifting_surface.n_spanwise_panels(); k++) {
                    // Use doublet panel - vortex ring equivalence.
                    velocity -= d->wake.vortex_ring_unit_velocity(surface, panel, k) * d->wake.doublet_coefficients[k];
                }
            }
        }
    }
    
    // Take normal component, and subtract blowing velocity:
    const Vector3d &normal = surface.panel_normal(panel);
    return velocity.dot(normal) - boundary_layer.blowing_velocity(panel);
}

/**
   Returns velocity potential value on the body surface.
   
   @returns Surface potential value.
*/
double
Solver::compute_surface_velocity_potential(const Surface &surface, int offset, int panel) const
{
    if (Parameters::marcov_surface_velocity) {
        // Since we use N. Marcov's formula for surface velocity, we also compute the surface velocity
        // potential directly.
        return velocity_potential(surface.panel_collocation_point(panel, false));
        
    } else {
        double phi = -doublet_coefficients(offset + panel);
        
        // Add flow potential due to kinematic velocity:
        Body *body = surface_id_to_body.find(surface.id)->second;
        Vector3d apparent_velocity = body->panel_kinematic_velocity(surface, panel) - freestream_velocity;
        
        phi -= apparent_velocity.dot(surface.panel_collocation_point(panel, false));
        
        return phi;
    }
}

/**
   Computes velocity potential time derivative at the given panel.
   
   @param[in]  surface_velocity_potentials            Current potential values.
   @param[in]  previous_surface_velocity_potentials   Previous potential values
   @param[in]  offset                                 Offset to requested Surface
   @param[in]  panel                                  Panel number.
   @param[in]  dt                                     Time step size.
   
   @returns Velocity potential time derivative.
*/ 
double
Solver::compute_surface_velocity_potential_time_derivative(int offset, int panel, double dt) const
{
    double dphidt;
    
    // Evaluate the time-derivative of the potential in a body-fixed reference frame, as in
    //   J. P. Giesing, Nonlinear Two-Dimensional Unsteady Potential Flow with Lift, Journal of Aircraft, 1968.
    if (Parameters::unsteady_bernoulli && dt > 0.0)
        dphidt = (surface_velocity_potentials(offset + panel) - previous_surface_velocity_potentials(offset + panel)) / dt;
    else
        dphidt = 0.0;
        
    return dphidt;
}

/**
   Computes the surface velocity for the given panel.
   
   @param[in]   surface                     Reference surface.
   @param[it]   offset                      Doublet coefficient vector offset.
   @param[in]   panel                       Reference panel.
   
   @returns Surface velocity.
*/
Eigen::Vector3d
Solver::compute_surface_velocity(const Surface &surface, int offset, int panel) const
{
    // Compute disturbance part of surface velocity.
    Vector3d tangential_velocity;
    if (Parameters::marcov_surface_velocity) {
        const Vector3d &x = surface.panel_collocation_point(panel, false);
        
        // Use N. Marcov's formula for surface velocity, see L. Dragoş, Mathematical Methods in Aerodynamics, Springer, 2003.
        Vector3d tangential_velocity = compute_disturbance_velocity(x);
        tangential_velocity -= 0.5 * surface.scalar_field_gradient(doublet_coefficients, offset, panel);
    } else
        tangential_velocity = -surface.scalar_field_gradient(doublet_coefficients, offset, panel);

    // Add flow due to kinematic velocity:
    Body *body = surface_id_to_body.find(surface.id)->second;
    Vector3d apparent_velocity = body->panel_kinematic_velocity(surface, panel) - freestream_velocity;
                                          
    tangential_velocity -= apparent_velocity;
    
    // Remove any normal velocity.  This is the (implicit) contribution of the source term.
    const Vector3d &normal = surface.panel_normal(panel);
    tangential_velocity -= tangential_velocity.dot(normal) * normal;
    
    // Done:
    return tangential_velocity;
}

/**
   Returns the square of the reference velocity for the given body.
   
   @param[in]   body   Body to establish reference velocity for.
   
   @returns Square of the reference velocity.
*/
double
Solver::compute_reference_velocity_squared(const Body &body) const
{
    return (body.velocity - freestream_velocity).squaredNorm();
}

/**
   Computes the pressure coefficient.
   
   @param[in]   surface_velocity   Surface velocity for the reference panel.
   @param[in]   dphidt             Time-derivative of the velocity potential for the reference panel.
   @param[in]   v_ref              Reference velocity.
   
   @returns Pressure coefficient.
*/
double
Solver::compute_pressure_coefficient(const Vector3d &surface_velocity, double dphidt, double v_ref_squared) const
{
    double C_p = 1 - (surface_velocity.squaredNorm() + 2 * dphidt) / v_ref_squared;
    
    return C_p;
}

/**
   Computes the disturbance velocity potential at the given point.
   
   @param[in]   x   Reference point.
   
   @returns Disturbance velocity potential.
*/
double
Solver::compute_disturbance_velocity_potential(const Vector3d &x) const
{
    double phi = 0.0;
    
    // Iterate all non-wake surfaces:
    int offset = 0;
    
    vector<Body::SurfaceData*>::const_iterator si;
    for (si = non_wake_surfaces.begin(); si != non_wake_surfaces.end(); si++) {
        const Body::SurfaceData *d = *si;

        for (int i = 0; i < d->surface.n_panels(); i++) {
            double source_influence, doublet_influence;
            
            d->surface.source_and_doublet_influence(x, i, source_influence, doublet_influence);
            
            phi += doublet_influence * doublet_coefficients(offset + i);
            phi += source_influence * source_coefficients(offset + i);
        }
        
        offset += d->surface.n_panels();
    }
    
    // Iterate wakes:
    vector<Body*>::const_iterator bi;
    for (bi = bodies.begin(); bi != bodies.end(); bi++) {
        const Body *body = *bi;
        
        vector<Body::LiftingSurfaceData*>::const_iterator lsi;
        for (lsi = body->lifting_surfaces.begin(); lsi != body->lifting_surfaces.end(); lsi++) {
            const Body::LiftingSurfaceData *d = *lsi;

            for (int i = 0; i < d->wake.n_panels(); i++)
                phi += d->wake.doublet_influence(x, i) * d->wake.doublet_coefficients[i];
        }
    }
                    
    // Done:
    return phi;
}

/**
   Computes disturbance potential gradient at the given point.
   
   @param[in]  x    Reference point.
   
   @returns Disturbance potential gradient.
*/ 
Eigen::Vector3d
Solver::compute_disturbance_velocity(const Eigen::Vector3d &x) const
{
    Vector3d gradient(0, 0, 0);
    
    // Iterate all non-wake surfaces:
    int offset = 0;
    
    vector<Body::SurfaceData*>::const_iterator si;
    for (si = non_wake_surfaces.begin(); si != non_wake_surfaces.end(); si++) {
        const Body::SurfaceData *d = *si;

        for (int i = 0; i < d->surface.n_panels(); i++) {
            gradient += d->surface.vortex_ring_unit_velocity(x, i) * doublet_coefficients(offset + i);
            gradient += d->surface.source_unit_velocity(x, i) * source_coefficients(offset + i);
        }
        
        offset += d->surface.n_panels();
    }
    
    // Iterate wakes:
    vector<Body*>::const_iterator bi;
    for (bi = bodies.begin(); bi != bodies.end(); bi++) {
        const Body *body = *bi;
        
        vector<Body::LiftingSurfaceData*>::const_iterator lsi;
        for (lsi = body->lifting_surfaces.begin(); lsi != body->lifting_surfaces.end(); lsi++) {
            const Body::LiftingSurfaceData *d = *lsi;
            
            if (d->wake.n_panels() >= d->lifting_surface.n_spanwise_panels()) {
                for (int i = 0; i < d->wake.n_panels(); i++)
                    gradient += d->wake.vortex_ring_unit_velocity(x, i) * d->wake.doublet_coefficients[i];
            }
        }
    }
               
    // Done:
    return gradient;
}

/**
   Computes the vector by which the first wake vortex is offset from the trailing edge.
   
   @param[in]   body              Reference body.
   @param[in]   lifting_surface   Reference lifting surface.
   @param[in]   index             Trailing edge index.
   @param[in]   dt                Time step size.
   
   @returns The trailing edge vortex displacement.
*/
Eigen::Vector3d
Solver::compute_trailing_edge_vortex_displacement(const Body &body, const LiftingSurface &lifting_surface, int index, double dt) const
{
    Vector3d apparent_velocity = body.node_kinematic_velocity(lifting_surface, lifting_surface.trailing_edge_node(index)) - freestream_velocity;
                    
    Vector3d wake_velocity;
    if (Parameters::wake_emission_follow_bisector)
        wake_velocity = apparent_velocity.norm() * lifting_surface.trailing_edge_bisector(index);
    else
        wake_velocity = -apparent_velocity;
    
    return Parameters::wake_emission_distance_factor * wake_velocity * dt;   
}
