#include <systemc>
#include <iostream>
#include <aff3ct.hpp>

int sc_main(int argc, char** argv)
{
	std::cout << "#-------------------------------------------------------" << std::endl;
	std::cout << "# This is a basic program using the AFF3CT library."      << std::endl;
	std::cout << "# Feel free to improve it as you want to fit your needs." << std::endl;
	std::cout << "#-------------------------------------------------------" << std::endl;
	std::cout << "#"                                                        << std::endl;

	const int   fe       = 100;
	const int   seed     = argc >= 2 ? std::atoi(argv[1]) : 0;
	const int   K        = 32;
	const int   N        = 128;
	const float R        = (float)K / (float)N;
	const float ebn0_min = 0.00f;
	const float ebn0_max = 10.1f;

	std::cout << "# * Simulation parameters: "           << std::endl;
	std::cout << "#    ** Frame errors   = " << fe       << std::endl;
	std::cout << "#    ** Noise seed     = " << seed     << std::endl;
	std::cout << "#    ** Info. bits (K) = " << K        << std::endl;
	std::cout << "#    ** Frame size (N) = " << N        << std::endl;
	std::cout << "#    ** Code rate  (R) = " << R        << std::endl;
	std::cout << "#    ** SNR min   (dB) = " << ebn0_min << std::endl;
	std::cout << "#    ** SNR max   (dB) = " << ebn0_max << std::endl;
	std::cout << "#"                                     << std::endl;

	using namespace aff3ct;

	// create the AFF3CT objects
	module::Source_random<>          source  (K      );
	module::Encoder_repetition_sys<> encoder (K, N   );
	module::Modem_BPSK<>             modem   (N      );
	module::Channel_AWGN_LLR<>       channel (N, seed);
	module::Decoder_repetition_std<> decoder (K, N   );
	module::Monitor_BFER<>           monitor (K, fe  );

	// create reporters to display results in terminal
	tools::Sigma<float>                  noise;
	std::vector<std::unique_ptr<tools::Reporter>> reporters;

	reporters.push_back(std::unique_ptr<tools::Reporter_noise<float>        >(new tools::Reporter_noise<float>        (noise  ))); // reporter of the noise value
	reporters.push_back(std::unique_ptr<tools::Reporter_BFER <int>          >(new tools::Reporter_BFER <int>          (monitor))); // reporter of the bit/frame error rate
	reporters.push_back(std::unique_ptr<tools::Reporter_throughput<uint64_t>>(new tools::Reporter_throughput<uint64_t>(monitor))); // reporter of the throughput of the simulation


	tools::Terminal_std terminal(reporters); // the terminal that display all the legend associated with the
	                                                 // reporters and the trace
	terminal.legend();


	// configuration of the tasks
	std::vector<const module::Module*> modules = {&source, &encoder, &modem, &channel, &decoder, &monitor};
	for (auto& m : modules)
		for (auto& t : m->tasks)
		{
			t->set_debug(false); // disable the debug mode
			t->set_stats(true ); // enable the statistics
		}

	// add a callback to the monitor to call the "sc_core::sc_stop()" function
	monitor.add_handler_check([&]() -> void
	{
		if (monitor.fe_limit_achieved() || tools::Terminal::is_interrupt())
			sc_core::sc_stop();
	});

	// a loop over the various SNRs
	using namespace module;
	for (auto ebn0 = ebn0_min; ebn0 < ebn0_max; ebn0 += 1.f)
	{
		// compute the current sigma for the channel noise
		const auto esn0  = tools::ebn0_to_esn0 (ebn0, R);
		const auto sigma = tools::esn0_to_sigma(esn0   );

		noise.set_noise(sigma, ebn0, esn0);

		// update the sigma of the modem and the channel
		modem  .set_noise(noise);
		channel.set_noise(noise);

		// create "sc_core::sc_module" instances for each task
		source .sc.create_module(+src::tsk::generate    );
		encoder.sc.create_module(+enc::tsk::encode      );
		modem  .sc.create_module(+mdm::tsk::modulate    );
		modem  .sc.create_module(+mdm::tsk::demodulate  );
		channel.sc.create_module(+chn::tsk::add_noise   );
		decoder.sc.create_module(+dec::tsk::decode_siho );
		monitor.sc.create_module(+mnt::tsk::check_errors);

		// declare a SystemC duplicator to duplicate the source module output
		tools::SC_Duplicator duplicator;

		// bind the sockets between the modules
		source .sc[+src::tsk::generate   ].s_out[+src::sck::generate   ::U_K ](duplicator                         .s_in                              );
		duplicator                        .s_out1                             (monitor.sc[+mnt::tsk::check_errors].s_in[+mnt::sck::check_errors::U   ]);
		duplicator                        .s_out2                             (encoder.sc[+enc::tsk::encode      ].s_in[+enc::sck::encode      ::U_K ]);
		encoder.sc[+enc::tsk::encode     ].s_out[+enc::sck::encode     ::X_N ](modem  .sc[+mdm::tsk::modulate    ].s_in[+mdm::sck::modulate    ::X_N1]);
		modem  .sc[+mdm::tsk::modulate   ].s_out[+mdm::sck::modulate   ::X_N2](channel.sc[+chn::tsk::add_noise   ].s_in[+chn::sck::add_noise   ::X_N ]);
		channel.sc[+chn::tsk::add_noise  ].s_out[+chn::sck::add_noise  ::Y_N ](modem  .sc[+mdm::tsk::demodulate  ].s_in[+mdm::sck::demodulate  ::Y_N1]);
		modem  .sc[+mdm::tsk::demodulate ].s_out[+mdm::sck::demodulate ::Y_N2](decoder.sc[+dec::tsk::decode_siho ].s_in[+dec::sck::decode_siho ::Y_N ]);
		decoder.sc[+dec::tsk::decode_siho].s_out[+dec::sck::decode_siho::V_K ](monitor.sc[+mnt::tsk::check_errors].s_in[+mnt::sck::check_errors::V   ]);

		// display the performance (BER and FER) in real time (in a separate thread)
		terminal.start_temp_report();

		// start the SystemC simulation
		sc_core::sc_report_handler::set_actions(sc_core::SC_INFO, sc_core::SC_DO_NOTHING);
		sc_core::sc_start();

		// display the performance (BER and FER) in the terminal
		terminal.final_report();

		if (tools::Terminal::is_over())
			break;

		// reset the monitor for the next SNR
		monitor.reset();
		tools::Terminal::reset();

		// dirty way to create a new SystemC simulation context
		sc_core::sc_curr_simcontext = new sc_core::sc_simcontext();
		sc_core::sc_default_global_context = sc_core::sc_curr_simcontext;
	}

	std::cout << "#" << std::endl;

	// display the statistics of the tasks (if enabled)
	auto ordered = true;
	tools::Stats::show(modules, ordered);

	std::cout << "# End of the simulation" << std::endl;

	return 0;
}
