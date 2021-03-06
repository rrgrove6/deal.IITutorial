/* ---------------------------------------------------------------------
 *
 * Copyright (C) 2009 - 2014 by the deal.II authors
 *
 * This file is part of the deal.II library.
 *
 * The deal.II library is free software; you can use it, redistribute
 * it, and/or modify it under the terms of the GNU Lesser General
 * Public License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 * The full text of the license can be found in the file LICENSE at
 * the top level of the deal.II distribution.
 *
 * ---------------------------------------------------------------------
 *
 * Author: Wolfgang Bangerth, Texas A&M University, 2009, 2010
 *         Timo Heister, University of Goettingen, 2009, 2010
 */


#include <deal.II/base/quadrature_lib.h>
#include <deal.II/base/function.h>
#include <deal.II/base/timer.h>

#include <deal.II/lac/generic_linear_algebra.h>
#include <string>

#define USE_PETSC_LA

namespace LA
{
#ifdef USE_PETSC_LA
using namespace dealii::LinearAlgebraPETSc;
#else
using namespace dealii::LinearAlgebraTrilinos;
#endif
}

#include <deal.II/lac/vector.h>
#include <deal.II/lac/full_matrix.h>
#include <deal.II/lac/solver_cg.h>
#include <deal.II/lac/constraint_matrix.h>
#include <deal.II/lac/compressed_simple_sparsity_pattern.h>

#include <deal.II/lac/petsc_parallel_sparse_matrix.h>
#include <deal.II/lac/petsc_parallel_vector.h>
#include <deal.II/lac/petsc_solver.h>
#include <deal.II/lac/petsc_precondition.h>

#include <deal.II/grid/grid_generator.h>
#include <deal.II/grid/tria_accessor.h>
#include <deal.II/grid/tria_iterator.h>
#include <deal.II/dofs/dof_handler.h>
#include <deal.II/dofs/dof_accessor.h>
#include <deal.II/dofs/dof_tools.h>
#include <deal.II/fe/fe_values.h>
#include <deal.II/fe/fe_q.h>
#include <deal.II/numerics/vector_tools.h>
#include <deal.II/numerics/data_out.h>
#include <deal.II/numerics/error_estimator.h>

#include <deal.II/base/utilities.h>
#include <deal.II/base/conditional_ostream.h>
#include <deal.II/base/index_set.h>
#include <deal.II/lac/sparsity_tools.h>
#include <deal.II/distributed/tria.h>
#include <deal.II/distributed/grid_refinement.h>
#include <deal.II/matrix_free/helper_functions.h>

#include <fstream>
#include <iostream>

namespace Step40
{
using namespace dealii;


template <int dim>
class LaplaceProblem
{
public:
	LaplaceProblem ();
	~LaplaceProblem ();

	void run ();

private:
	void setup_system ();
	void assemble_system (bool for_u);
	void solve (bool for_u);
	void refine_grid ();
	void output_results (const unsigned int cycle, int count) const;

	MPI_Comm                                  mpi_communicator;

	parallel::distributed::Triangulation<dim> triangulation;

	DoFHandler<dim>                           dof_handler;
	FE_Q<dim>                                 fe;

	IndexSet                                  locally_owned_dofs;
	IndexSet                                  locally_relevant_dofs;

	ConstraintMatrix                          constraints_u;
	ConstraintMatrix                          constraints_v;

	LA::MPI::SparseMatrix system_matrix;
	LA::MPI::Vector locally_relevant_solution_u;
	LA::MPI::Vector locally_relevant_solution_v;
	LA::MPI::Vector old_locally_relevant_solution_u;
	LA::MPI::Vector old_locally_relevant_solution_v;
	LA::MPI::Vector system_rhs;
	LA::MPI::Vector number_of_locally_owned_dofs;
	LA::MPI::Vector distributed_solution;

	double time, time_step;
	unsigned int timestep_number;

	ConditionalOStream                        pcout;
	TimerOutput                               computing_timer;
	double                                    theta;
	bool									  for_u;
	int                                       count;
};

template <int dim>
class InitialValuesU : public Function<dim>
{
public:
  InitialValuesU () : Function<dim>() {}
  virtual double value (const Point<dim>   &p,
                        const unsigned int  component = 0) const;
};
template <int dim>
class InitialValuesV : public Function<dim>
{
public:
  InitialValuesV () : Function<dim>() {}
  virtual double value (const Point<dim>   &p,
                        const unsigned int  component = 0) const;
};
template <int dim>
double InitialValuesU<dim>::value (const Point<dim>  &p,
                                   const unsigned int component) const
{
  Assert (component == 0, ExcInternalError());
  return 0;
}
template <int dim>
double InitialValuesV<dim>::value (const Point<dim>  &p,
                                   const unsigned int component) const
{
  Assert (component == 0, ExcInternalError());
  return 0;
}

template <int dim>
class BoundaryValuesU : public Function<dim>
{
public:
	BoundaryValuesU () : Function<dim>() {}
	virtual double value (const Point<dim>   &p,
			const unsigned int  component = 0) const;
};
template <int dim>
class BoundaryValuesV : public Function<dim>
{
public:
	BoundaryValuesV () : Function<dim>() {}
	virtual double value (const Point<dim>   &p,
			const unsigned int  component = 0) const;
};
template <int dim>
double BoundaryValuesU<dim>::value (const Point<dim> &p,
		const unsigned int component) const
		{
	Assert (component == 0, ExcInternalError());
	if ((this->get_time() <= 0.5) &&
			(p[0] < 0) &&
			(p[1] < 1./3) &&
			(p[1] > -1./3))
		return std::sin (this->get_time() * 4 * numbers::PI);
	else
		return 0;
		}
template <int dim>
double BoundaryValuesV<dim>::value (const Point<dim> &p,
		const unsigned int component) const
		{
	Assert (component == 0, ExcInternalError());
	if ((this->get_time() <= 0.5) &&
			(p[0] < 0) &&
			(p[1] < 1./3) &&
			(p[1] > -1./3))
		return (std::cos (this->get_time() * 4 * numbers::PI) *
				4 * numbers::PI);
	else
		return 0;
		}


template <int dim>
LaplaceProblem<dim>::LaplaceProblem ()
:
mpi_communicator (MPI_COMM_WORLD),
triangulation (mpi_communicator,
		typename Triangulation<dim>::MeshSmoothing
		(Triangulation<dim>::smoothing_on_refinement |
				Triangulation<dim>::smoothing_on_coarsening)),
				dof_handler (triangulation),
				fe (2),
				pcout (std::cout,
						(Utilities::MPI::this_mpi_process(mpi_communicator)
== 0)),
computing_timer (mpi_communicator,
		pcout,
		TimerOutput::summary,
		TimerOutput::wall_times),
		time_step (1./256),
		theta (0.5)
		{}



template <int dim>
LaplaceProblem<dim>::~LaplaceProblem ()
{
	dof_handler.clear ();
}



template <int dim>
void LaplaceProblem<dim>::setup_system ()
{
	TimerOutput::Scope t(computing_timer, "setup");

	dof_handler.distribute_dofs (fe);

	locally_owned_dofs = dof_handler.locally_owned_dofs ();

	// std::cout << "My number of DOF: " << locally_owned_dofs.n_elements() << std::endl;

	DoFTools::extract_locally_relevant_dofs (dof_handler,
			locally_relevant_dofs);

	locally_relevant_solution_u.reinit (locally_owned_dofs,
			locally_relevant_dofs, mpi_communicator);
	locally_relevant_solution_v.reinit (locally_owned_dofs,
			locally_relevant_dofs, mpi_communicator);
	old_locally_relevant_solution_u.reinit (locally_owned_dofs,
			locally_relevant_dofs, mpi_communicator);
	old_locally_relevant_solution_v.reinit (locally_owned_dofs,
			locally_relevant_dofs, mpi_communicator);
	system_rhs.reinit (locally_owned_dofs, mpi_communicator);

	distributed_solution.reinit (locally_owned_dofs, mpi_communicator);
	VectorTools::interpolate (dof_handler,
			InitialValuesU<dim>(),
			distributed_solution);
	locally_relevant_solution_u = distributed_solution;
	old_locally_relevant_solution_u = distributed_solution;
	VectorTools::interpolate (dof_handler,
			InitialValuesV<dim>(),
			distributed_solution);
	locally_relevant_solution_v = distributed_solution;
	old_locally_relevant_solution_v = distributed_solution;

	// todo: hanging node cosntraints for initial condition

	constraints_u.clear ();
	constraints_u.reinit (locally_relevant_dofs);

	time = 0;
	BoundaryValuesU<dim> boundary_values_u_function;
	boundary_values_u_function.set_time (time);
	DoFTools::make_hanging_node_constraints (dof_handler, constraints_u);
	VectorTools::interpolate_boundary_values (dof_handler,
			0,
			boundary_values_u_function,
			constraints_u);

	constraints_u.close ();

	constraints_v.clear ();
	constraints_v.reinit (locally_relevant_dofs);

	BoundaryValuesU<dim> boundary_values_v_function;
	boundary_values_v_function.set_time (time);
	DoFTools::make_hanging_node_constraints (dof_handler, constraints_v);
	VectorTools::interpolate_boundary_values (dof_handler,
			0,
			boundary_values_v_function,
			constraints_v);

	constraints_v.close ();

	CompressedSimpleSparsityPattern csp (locally_relevant_dofs);

	DoFTools::make_sparsity_pattern (dof_handler, csp,
			constraints_v, false);
	SparsityTools::distribute_sparsity_pattern (csp,
			dof_handler.n_locally_owned_dofs_per_processor(),
			mpi_communicator,
			locally_relevant_dofs);

	system_matrix.reinit (locally_owned_dofs,
			locally_owned_dofs,
			csp,
			mpi_communicator);
}




template <int dim>
void LaplaceProblem<dim>::assemble_system (bool for_u)
{
	system_matrix = 0;
	system_rhs = 0;

	TimerOutput::Scope t(computing_timer, "assembly");

	const QGauss<dim>  quadrature_formula(3);

	FEValues<dim> fe_values (fe, quadrature_formula,
			update_values    |  update_gradients |
			update_quadrature_points |
			update_JxW_values);

	const unsigned int   dofs_per_cell = fe.dofs_per_cell;
	const unsigned int   n_q_points    = quadrature_formula.size();

	FullMatrix<double>   cell_matrix (dofs_per_cell, dofs_per_cell);
	Vector<double>       cell_rhs (dofs_per_cell);

	std::vector<types::global_dof_index> local_dof_indices (dofs_per_cell);

	double lhs_term = 0.0;
	double rhs_term = 0.0;

	std::vector< double> solution_u_values;
	std::vector< double> old_solution_u_values;
	std::vector< double> old_solution_v_values;
	std::vector< Tensor< 1, dim > > solution_u_grad;
	std::vector< Tensor< 1, dim > > old_solution_u_grad;

	typename DoFHandler<dim>::active_cell_iterator
	cell = dof_handler.begin_active(),
	endc = dof_handler.end();
	for (; cell!=endc; ++cell)
		if (cell->is_locally_owned())
		{
			cell_matrix = 0;
			cell_rhs = 0;

			fe_values.reinit (cell);

			solution_u_values.resize(n_q_points);
			old_solution_u_values.resize(n_q_points);
			old_solution_v_values.resize(n_q_points);
			solution_u_grad.resize(n_q_points);
			old_solution_u_grad.resize(n_q_points);

			fe_values.get_function_values(locally_relevant_solution_u ,solution_u_values);
			fe_values.get_function_values(old_locally_relevant_solution_u ,old_solution_u_values);
			fe_values.get_function_values(old_locally_relevant_solution_v ,old_solution_v_values);
			fe_values.get_function_gradients(locally_relevant_solution_u ,solution_u_grad);
			fe_values.get_function_gradients(old_locally_relevant_solution_u ,old_solution_u_grad);

			for (unsigned int q_point=0; q_point<n_q_points; ++q_point)
			{

				double rhs_value = 0;
				double old_rhs_value = 0;
				double a = 1.0; // could depend on q_point

				        //              double a = ( sqrt(fe_values.quadrature_point(q_point)[0] * fe_values.quadrature_point(q_point)[0] +
						//            		  fe_values.quadrature_point(q_point)[1] * fe_values.quadrature_point(q_point)[1])
						//                   <
						//                   .25
						//                   ? 100 : 1);


				for (unsigned int i=0; i<dofs_per_cell; ++i)
				{

					for (unsigned int j=0; j<dofs_per_cell; ++j)
					{
						if (for_u == true)
							lhs_term = time_step * time_step *
							theta * theta * a *
							fe_values.shape_grad(i,q_point) *
							fe_values.shape_grad(j,q_point) *
							fe_values.JxW(q_point);
						else
							lhs_term = 0.0;

						cell_matrix(i,j) += ((fe_values.shape_value(i,q_point) *
								fe_values.shape_value(j,q_point)) *
								fe_values.JxW(q_point) +
								lhs_term);
					}

					if(for_u == true)
						rhs_term = (
								fe_values.shape_value(i,q_point) *
								old_solution_u_values[q_point]
								                      -
								                      time_step * time_step *
								                      theta * (1 - theta) *
								                      fe_values.shape_grad(i,q_point) *
								                      old_solution_u_grad[q_point]
								                                          +
								                                          time_step *
								                                          fe_values.shape_value(i,q_point) *
								                                          old_solution_v_values[q_point]
								                                                                -
								                                                                time_step * time_step *
								                                                                theta *
								                                                                (
								                                                                		theta *
								                                                                		rhs_value *
								                                                                		fe_values.shape_value(i,q_point)
								                                                                		+
								                                                                		(1 - theta) *
								                                                                		old_rhs_value *
								                                                                		fe_values.shape_value(i,q_point)
								                                                                )
						) *
						fe_values.JxW(q_point);
					else
						rhs_term = (
								fe_values.shape_value(i,q_point) *
								old_solution_v_values[q_point]
								                      -
								                      time_step *
								                      (
								                    		  theta*
								                    		  fe_values.shape_grad(i,q_point) *
								                    		  solution_u_grad[q_point]
								                    		                  +
								                    		                  (1 - theta) *
								                    		                  fe_values.shape_grad(i,q_point) *
								                    		                  old_solution_u_grad[q_point]
								                      )
								                      +
								                      time_step *
								                      (
								                    		  theta *
								                    		  rhs_value *
								                    		  fe_values.shape_value(i,q_point)
								                    		  +
								                    		  (1 - theta) *
								                    		  old_rhs_value *
								                    		  fe_values.shape_value(i,q_point)
								                      )
						) *
						fe_values.JxW(q_point);
					cell_rhs(i) += rhs_term;
				}

			}

			cell->get_dof_indices (local_dof_indices);

			constraints_u.distribute_local_to_global (cell_matrix,
					cell_rhs,
					local_dof_indices,
					system_matrix,
					system_rhs);
			constraints_v.distribute_local_to_global (cell_matrix,
					cell_rhs,
					local_dof_indices,
					system_matrix,
					system_rhs);
		}

	system_matrix.compress (VectorOperation::add);
	system_rhs.compress (VectorOperation::add);


	// std::cout << "Number of locally owned cells  " << count << std::endl;
}



template <int dim>
void LaplaceProblem<dim>::solve(bool for_u)
{
	TimerOutput::Scope t(computing_timer, "solve");

	LA::MPI::Vector
	completely_distributed_solution (locally_owned_dofs, mpi_communicator);

	SolverControl solver_control (dof_handler.n_dofs(), 1e-12);

	LA::SolverCG solver(solver_control, mpi_communicator);
	LA::MPI::PreconditionAMG preconditioner;

	LA::MPI::PreconditionAMG::AdditionalData data;

#ifdef USE_PETSC_LA
	data.symmetric_operator = true;
#else
	/* Trilinos defaults are good */
#endif
	preconditioner.initialize(system_matrix, data);


	solver.solve (system_matrix, completely_distributed_solution, system_rhs,
			preconditioner);


	pcout << "   Solved in " << solver_control.last_step()
        		  << " iterations." << std::endl;

	if (for_u == true)
	{
		constraints_u.distribute (completely_distributed_solution);
		old_locally_relevant_solution_u = locally_relevant_solution_u;
		locally_relevant_solution_u = completely_distributed_solution;
	}
	else
	{
		constraints_v.distribute (completely_distributed_solution);
		old_locally_relevant_solution_v = locally_relevant_solution_v;
		locally_relevant_solution_v = completely_distributed_solution;
	}
}



template <int dim>
void LaplaceProblem<dim>::refine_grid ()
{
	TimerOutput::Scope t(computing_timer, "refine");

	Vector<float> estimated_error_per_cell (triangulation.n_active_cells());
	KellyErrorEstimator<dim>::estimate (dof_handler,
			QGauss<dim-1>(3),
			typename FunctionMap<dim>::type(),
			locally_relevant_solution_u,
			estimated_error_per_cell);
	parallel::distributed::GridRefinement::
	refine_and_coarsen_fixed_number (triangulation,
			estimated_error_per_cell,
			0.3, 0.03);
	triangulation.execute_coarsening_and_refinement ();
}




template <int dim>
void LaplaceProblem<dim>::output_results (const unsigned int cycle, int count) const
{
  DataOut<dim> data_out;
  data_out.attach_dof_handler (dof_handler);
  data_out.add_data_vector (locally_relevant_solution_u, "u");
  data_out.add_data_vector (locally_relevant_solution_v, "v");

  Vector<float> subdomain (triangulation.n_active_cells());
  for (unsigned int i=0; i<subdomain.size(); ++i)
    subdomain(i) = triangulation.locally_owned_subdomain();
  data_out.add_data_vector (subdomain, "subdomain");

  data_out.build_patches ();

  const std::string filename = ("solution-" +
                                Utilities::int_to_string (cycle, 2) +
                                "." +
                                Utilities::int_to_string(triangulation.locally_owned_subdomain(), 4) +
                                "." +
                                Utilities::int_to_string (count, 2));
  std::ofstream output ((filename + ".vtu").c_str());
  data_out.write_vtu (output);

  if (Utilities::MPI::this_mpi_process(mpi_communicator) == 0)
    {
      std::vector<std::string> filenames;
      for (unsigned int i=0;
           i<Utilities::MPI::n_mpi_processes(mpi_communicator);
           ++i)
        filenames.push_back ("solution-" +
                             Utilities::int_to_string (cycle, 2) +
                             "." +
                             Utilities::int_to_string (i, 4) +
                             "." +
                             Utilities::int_to_string (count, 2) +
                             ".vtu");

      std::ofstream master_output (("solution-" + Utilities::int_to_string (count, 2) + ".pvtu").c_str());
      data_out.write_pvtu_record (master_output, filenames);
    }
}





template <int dim>
void LaplaceProblem<dim>::run ()
{
	//    const unsigned int n_cycles = 8;
	//    for (unsigned int cycle=0; cycle<n_cycles; ++cycle)
	//      {
	//        pcout << "Cycle " << cycle << ':' << std::endl;
	//
	//        if (cycle == 0)
	//          {
	GridGenerator::hyper_cube (triangulation, -1, 1);
	triangulation.refine_global (6);
	//          }
	//        else
	//          refine_grid ();

	setup_system ();

	pcout << "   Number of active cells:       "
			<< triangulation.n_global_active_cells()
			<< std::endl
			<< "   Number of degrees of freedom: "
			<< dof_handler.n_dofs()
			<< std::endl;

	count = 1;
	for (timestep_number=1, time=time_step;
			time<=100; //temporary
			time+=time_step, ++timestep_number)
	{
		std::cout << "Time step " << timestep_number
				<< " at t=" << time
				<< std::endl;

		constraints_u.clear ();
		constraints_u.reinit (locally_relevant_dofs);

		BoundaryValuesU<dim> boundary_values_u_function;
		boundary_values_u_function.set_time (time);
		DoFTools::make_hanging_node_constraints (dof_handler, constraints_u);
		VectorTools::interpolate_boundary_values (dof_handler,
				0,
				boundary_values_u_function,
				constraints_u);

		constraints_u.close ();

		constraints_v.clear ();
		constraints_v.reinit (locally_relevant_dofs);

		BoundaryValuesU<dim> boundary_values_v_function;
		boundary_values_v_function.set_time (time);
		DoFTools::make_hanging_node_constraints (dof_handler, constraints_v);
		VectorTools::interpolate_boundary_values (dof_handler,
				0,
				boundary_values_v_function,
				constraints_v);

		constraints_v.close ();

		for_u = true;
		assemble_system (for_u);
		solve (for_u);

		for_u = false;
		assemble_system (for_u);
		solve (for_u);

		int cycle = 1; // temporary
		if (Utilities::MPI::n_mpi_processes(mpi_communicator) <= 32)
		{
			TimerOutput::Scope t(computing_timer, "output");
			output_results (cycle, count);
		}

		computing_timer.print_summary ();
		computing_timer.reset ();

		pcout << std::endl;
		count =  count + 1;
	}
}
}




int main(int argc, char *argv[])
{
	try
	{
		using namespace dealii;
		using namespace Step40;

		Utilities::MPI::MPI_InitFinalize mpi_initialization(argc, argv, 1);
		deallog.depth_console (0);

		{
			LaplaceProblem<2> laplace_problem_2d;
			laplace_problem_2d.run ();
		}
	}
	catch (std::exception &exc)
	{
		std::cerr << std::endl << std::endl
				<< "----------------------------------------------------"
				<< std::endl;
		std::cerr << "Exception on processing: " << std::endl
				<< exc.what() << std::endl
				<< "Aborting!" << std::endl
				<< "----------------------------------------------------"
				<< std::endl;

		return 1;
	}
	catch (...)
	{
		std::cerr << std::endl << std::endl
				<< "----------------------------------------------------"
				<< std::endl;
		std::cerr << "Unknown exception!" << std::endl
				<< "Aborting!" << std::endl
				<< "----------------------------------------------------"
				<< std::endl;
		return 1;
	}

	return 0;
}
