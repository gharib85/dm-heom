// This file is part of DM-HEOM (https://github.com/noma/dm-heom)
//
// Copyright (c) 2015-2019 Matthias Noack, Zuse Institute Berlin
//
// Licensed under the 3-clause BSD License, see accompanying LICENSE,
// CONTRIBUTORS.md, and README.md for further information.

#include <exception>
#include <iostream>

#include <boost/format.hpp>
#include <noma/num/meta_stepper.hpp>

#include "heom/command_line.hpp"
#include "heom/common.hpp"
#include "heom/handle_main_exception.hpp"
#include "heom/hierarchy_graph.hpp"
#include "heom/hierarchy_mask.hpp"
#include "heom/hierarchy_norm.hpp"
#include "heom/instance.hpp"
#include "heom/make_file_observer_list.hpp"
#include "heom/ocl_config.hpp"
#include "heom/ode.hpp"
#include "heom/population_dynamics_config.hpp"
#include "heom/population_dynamics_solver.hpp"

namespace bmt = ::heom::bmt;
namespace num = ::heom::num;
namespace ocl = ::heom::ocl;
using num::int_t;
using num::real_t;

int main(int argc, char* argv[])
{
	// start runtime measurement
	bmt::timer app_timer;

	// output compile-time configuration
	std::cout << "-------------------- Compile-Time Configuration ------------" << std::endl;
	heom::write_compile_config(std::cout);
	std::cout << "------------------------------------------------------------" << std::endl;

	std::exception_ptr eptr;
	try {
		// command line parsing
		heom::command_line command_line { argc, argv };

		// parse config files
		std::cout << "Parsing OpenCL configuration from file: " << command_line.ocl_config_filename() << std::endl;
		heom::ocl_config ocl_config(command_line.ocl_config_filename());
		std::cout << "Parsing HEOM configuration from file: " << command_line.heom_config_filename() << std::endl;
		heom::population_dynamics_config heom_config(command_line.heom_config_filename());

		heom::hierarchy_graph complete_graph(heom_config.baths_number(), heom_config.baths_matsubaras(), heom_config.system_ado_depth());

		heom::instance heom_instance(heom_config, heom::sites_to_states_mode_t::identity, complete_graph);
		heom_instance.set_hierarchy_top(heom_config.population_dynamics_rho_init().data());

		using stepper_t = num::meta_stepper; // uses solver_stepper_type from config
		using solver_t = heom::population_dynamics_solver<heom::ode, stepper_t, heom::hierarchy_norm>;

		// create a solver from configuration and instance
		const ocl::nd_range ocl_nd_range {
				{}, // offset
		        {1, static_cast<std::uint64_t>(heom_instance.matrices())}, // global size
		        {1, 1} // local size
			};

		solver_t solver(ocl_config, ocl_nd_range, heom_config, heom_instance);
		std::cout << "-------------------- OpenCL Runtime Configuration ----------" << std::endl;
		solver.write_ocl_runtime_config(std::cout);
		std::cout << "------------------------------------------------------------" << std::endl;

		// with create_hierarchy_mask we create mask corresponding to configured filtering strategy, then update solver
		// NOTE: .data() returns pointer to mask_t array
		heom::hierarchy_mask hierarchy_mask = heom::create_hierarchy_mask(heom_config.filtering_strategy(), complete_graph, heom_config.baths_matsubaras(), heom_config.filtering_first_layer());
		solver.update_hierarchy_mask(hierarchy_mask.data());

		std::cout << "-------------------- Hierarchy Mask Counter ----------------" << std::endl;
		heom::write_hierarchy_mask_stats(hierarchy_mask, std::cout);
		std::cout << "------------------------------------------------------------" << std::endl;

		// setup output
		heom::observer_list observer_list = heom::make_file_observer_list(heom_config, complete_graph, solver);
		observer_list.observe(0.0);

		// run the solver, print output every n steps
		int_t iterations = heom_config.solver_steps() / heom_config.program_observe_steps();
		int_t steps_per_iteration = heom_config.program_observe_steps();
		int_t total_steps = iterations * heom_config.program_observe_steps();

		for (int_t i = 0; i < iterations; ++i) {
			// propagate
			solver.step_forward(steps_per_iteration);

			// after propagation, we are at:
			const auto current_step = (i + 1) * steps_per_iteration;
			const real_t current_time = current_step * heom_config.solver_step_size();

			// observe
//			solver.get_top_result(result_buffer_top);
			observer_list.observe(current_time);

			// update status
			heom::write_progress(current_step - 1, total_steps, "Calculation population dynamics: ", std::cout);
		}

		// output benchmark results
		std::cout << "-------------------- Solver Runtime Summary ----------------" << std::endl;
		solver.write_runtime_summary(std::cout);
		std::cout << "------------------------------------------------------------" << std::endl;
		std::cout << "-------------------- Memory Summary ------------------------" << std::endl;
		std::cout << "local hierarchy buffer size:            "
		          << boost::format("%11.2f MiB\n") % (heom_instance.size_hierarchy_byte() / 1024.0 / 1024.0);
		std::cout << "local instance size:                    "
		          << boost::format("%11.2f MiB\n") % (heom_instance.allocated_byte() / 1024.0 / 1024.0);
		std::cout << "max. heap via aligned::allocate(..):    "
		          << boost::format("%11.2f MiB") % (heom::memory::aligned::instance().allocated_byte_max() / 1024.0 / 1024.0)
		          << " ("
		          << boost::format("count: %6i") % heom::memory::aligned::instance().allocations_max()
		          << ')' << std::endl;
		std::cout << "max. OpenCL buffer allocations:         "
		          << boost::format("%11.2f MiB") % (solver.ocl_helper().allocated_byte_max() / 1024.0 / 1024.0)
		          << " ("
			      << boost::format("count: %6i") % solver.ocl_helper().allocations_max()
			      << ')' << std::endl;
		std::cout << "------------------------------------------------------------" << std::endl;

	} catch (...) {
		eptr = std::current_exception();
	}
	int ret = heom::handle_main_exception(eptr);

	// print application runtime
	std::cout << "main(): application runtime: " << boost::format("%11.2f") % std::chrono::duration_cast<bmt::seconds>(bmt::duration(app_timer.elapsed())).count() << " s" << std::endl;

	return ret;
}
