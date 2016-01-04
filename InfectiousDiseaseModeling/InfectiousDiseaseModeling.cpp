#include <omp.h>
#include <iostream>
#include "Individual.h"
#include "GraphHandler.h"
#include "Settings.h"

using namespace std;
using namespace boost;

void simulate_parallel(int individual_count, std::uint8_t total_epochs, const LocationUndirectedGraph& individual_graph, vector<Individual>& individuals, vector<std::tuple<int, int>>& epoch_statistics) {

	int index = 0;
	static int max_index = static_cast<int>(individuals.size());
	static int chunk = static_cast<int>(max_index / DEFAULT_NUMBER_OF_THREADS);

	// Generate a look up map with the neighbouring nodes for each graph node
	map<int, vector<int>> neighborhood_lookup_map = GraphHandler::get_node_neighborhood_lookup_map(individual_graph);

	// Repeat for all the epochs
	for (std::uint8_t current_epoch = 0; current_epoch < (total_epochs + 1); ++current_epoch) {

		//	Randomly move all individuals
		#pragma omp parallel private(index) shared(individuals, neighborhood_lookup_map) firstprivate(chunk, max_index)
		{
			#pragma omp for schedule(static, chunk) nowait
			for (index = 0; index < max_index; ++index) {

				Individual current_individual = individuals[index]; // Thread local variable
				int current_location = current_individual.get_location(); // Thread local variable
				vector<int> neighborhood = neighborhood_lookup_map[current_location]; // Thread local variable, get the location's neighbourhood
				current_individual.move(neighborhood); // Stay in the same spot or move to a neighbouring node

				individuals[index] = current_individual; // Save individual back to the shared memory space
			}
		} // Implicit Barrier
			
		// Try to infect individuals that are close to infected ones
		#pragma omp parallel private(index) shared(individuals) firstprivate(chunk, max_index)
		{
			// Since we only change individuals that are "chunked" by index for each thread, there is no need for critical/atomic region
			#pragma omp for schedule(static,chunk) nowait
			for (index = 0; index < max_index; ++index) {
				if (!individuals[index].is_infected()) { // Don't copy the shared memory element, just check a boolean
					Individual current_individual = individuals[index]; // Thread local variable
					int affecting_index;
					for (affecting_index = 0; affecting_index < individual_count; ++affecting_index) {

						if (individuals[affecting_index].is_infected()) { // First do the binary check, then do the comparison because it is faster
							Individual affecting_individual = individuals[affecting_index]; // Thread local variable
							if (current_individual.get_location() == affecting_individual.get_location()) { // Now do the "expensive" comparison
								current_individual.try_infect();
								if (current_individual.is_infected()) { // Don't save to shared memory if the invidual wasn't eventually infected
									individuals[index] = current_individual; // Save affecting individual back to the shared memory space
									break; // No need to find other infected individuals in the same location, move the the next one
								}
							}
						}
					}
				}
			}

		} // Implicit Barrier

		// Advance the epoch for every individual and gather infected & hit statistics
		int hit_count = 0;
		int infected_count = 0;
		#pragma omp parallel private(index) shared(individuals) firstprivate(chunk, max_index) reduction(+:infected_count, hit_count)
		{
			// Since we only change individuals that are "chunked" by index for each thread, there is no need for critical/atomic region
			#pragma omp for schedule(static, chunk) nowait
			for (index = 0; index < max_index; ++index) {		
				Individual current_individual = individuals[index]; // Thread local variable
				current_individual.advance_epoch();	// Check individuals for the number of epochs they're infected and tag them as healed and recovered if a threshold disease_duration is passed					
				individuals[index] = current_individual; // Save individual back to the shared memory space
				// Near the end of the parallel region, perform reduction. Gather statistics about the current advance_epoch : what is the fraction of infected and hit individual
				if (current_individual.is_infected()) {
					++infected_count;
					++hit_count;
				} else if (current_individual.is_hit())
					++hit_count;
			}
		} // Implicit Barrier

		epoch_statistics.push_back(std::make_tuple(hit_count, infected_count)); // Store tuple of statistics for the current epoch
	}

	if (SAVE_CSV)
		GraphHandler::save_epoch_statistics_to_csv("output.csv", epoch_statistics);
	if (SAVE_GRAPHVIZ)
		GraphHandler::save_undirected_graph_to_graphviz_file("individualGraph.dot", individual_graph);
	if (SHOW_EPIDEMIC_RESULTS)
		GraphHandler::show_epidemic_results(individual_count, epoch_statistics);
}

void simulate_serial(int individual_count, int total_epochs, const LocationUndirectedGraph& individual_graph, vector<Individual>& individuals) {
	
	// Statistics vector, index is epoch
	vector<std::tuple<int, int>> epoch_statistics;
	
	// Generate a look up map with the neighbouring nodes for each graph node
	map<int, vector<int>> neighborhood_lookup_map = GraphHandler::get_node_neighborhood_lookup_map(individual_graph);

	// Repeat for all the epochs
	for (int current_epoch = 0; current_epoch < (total_epochs + 1); ++current_epoch) {
		
		//	Randomly move all individuals
		for (Individual& current_individual : individuals)
			current_individual.move(neighborhood_lookup_map[current_individual.get_location()]); // Stay in the same spot or move to a neighbouring node
		
		// foreach each individual		
		for (int individual_index = 0; individual_index != individuals.size(); ++individual_index) {			
			
			if (individuals[individual_index].is_infected()) { // if the individual is infected

				// and meets another individual that is susceptible the disease
				for (int affecting_individual = 0; affecting_individual != individuals.size(); ++affecting_individual) {
					if (individual_index != affecting_individual) {

						// Check if the susceptible individual gets infected
						if (individuals[individual_index].get_location() == individuals[affecting_individual].get_location()) // in the same location
							individuals[affecting_individual].try_infect();
					}
				}
			}
		}
		
		int hit_count = 0;
		int infected_count = 0;
		for (Individual& current_individual : individuals) {
			// Check individuals for the number of epochs they're infected and tag them as healed and recovered if a threshold disease_duration is passed
			current_individual.advance_epoch();

			// Gather statistics about the current advance_epoch : what is the fraction of infected and hit individual
			if (current_individual.is_infected())
				++infected_count;
			if (current_individual.is_hit())
				++hit_count;
		}
		epoch_statistics.push_back(std::make_tuple(hit_count, infected_count));
	}
	
	if (SAVE_CSV)
		GraphHandler::save_epoch_statistics_to_csv("output.csv", epoch_statistics);
	if (SAVE_GRAPHVIZ)
		GraphHandler::save_undirected_graph_to_graphviz_file("individualGraph.dot", individual_graph);
	if (SHOW_EPIDEMIC_RESULTS)
		GraphHandler::show_epidemic_results(individual_count, epoch_statistics);
}

void reset_input(string filename, int individual_count, int& location_count, int& edge_count, LocationUndirectedGraph& individual_graph, vector<Individual>& individuals) {
	individual_graph = GraphHandler::get_location_undirected_graph_from_file(filename); // Read graph from File OR
	//individual_graph = GraphHandler::get_sample_location_undirected_graph(); // Generate sample graph

	location_count = individual_graph.m_vertices.size();
	edge_count = individual_graph.m_edges.size();

	individuals = GraphHandler::get_random_individuals(individual_count, location_count); // Randomize positions of individuals

	// Infect initial individuals
	for (int i = 0; i < INITIAL_INFECTED_COUNT; ++i) {
		individuals[i].infect();
	}
}

int main() {

	// Get the default simulation values
	int thread_count = DEFAULT_NUMBER_OF_THREADS;
	int individual_count = DEFAULT_INDIVIDUAL_COUNT;
	std::uint8_t total_epochs = DEFAULT_TOTAL_EPOCHS;
	std::uint8_t repeat_count = DEFAULT_REPEAT_COUNT;
	string input_graph_filename = "antwerp.edges";//"minimumantwerp.edges"; // Read locations from the full Antwerp graph or from a minimal version (500 nodes)

	//individual_count *= 10;
	individual_count = 5000; // population of Antwerp is 503138
	//total_epochs *= 5;
	total_epochs = 1; // 30 days
	thread_count = 4;
	//repeat_count *= 4;
	repeat_count = 1;

	// Set the thread count
	omp_set_num_threads(thread_count);
	
	// Print calculation info
	std::cout << "----- Infectious Diease Modelling -----" << std::endl;
	std::cout << "Number of threads: " << thread_count << std::endl;
	std::cout << "Individual Count: " << individual_count << std::endl;
	std::cout << "Total Epochs: " << static_cast<int>(total_epochs) << std::endl;
	std::cout << "Graph from file: " << input_graph_filename << std::endl;
	std::cout << "Repeat count: " << static_cast<int>(repeat_count) << std::endl;

	LocationUndirectedGraph individual_graph; //Graph of location nodes & connections
	int location_count, edge_count;	
	vector<Individual> individuals; // Population of healthy individuals
	vector<std::tuple<int, int>> epoch_statistics;
	
	// Reset individuals
	reset_input(input_graph_filename, individual_count, location_count, edge_count, individual_graph, individuals);
	std::cout << "Location Count: " << location_count << std::endl; // print info once
	std::cout << "Edge Count: " << edge_count << std::endl; // print info once

	double time_start, time_end, total_time;
	
	// Serial
	cout << endl << "Running serial...";
	total_time = 0.0;
	for (std::uint8_t current_repeat = 0; current_repeat != repeat_count; ++current_repeat) {
		reset_input(input_graph_filename, individual_count, location_count, edge_count, individual_graph, individuals); // Reset individuals
		time_start = omp_get_wtime();
		simulate_serial(individual_count, total_epochs, individual_graph, individuals);
		time_end = omp_get_wtime() - time_start;
		total_time += time_end;
		cout << ".";
	}
	cout << (total_time / repeat_count) * 1000.0 << " ms" << endl;

	// OpenMP
	cout << endl << "Running with OpenMP...";
	total_time = 0.0;
	for (std::uint8_t current_repeat = 0; current_repeat != repeat_count; ++current_repeat) {
		reset_input(input_graph_filename, individual_count, location_count, edge_count, individual_graph, individuals); // Reset individuals
		time_start = omp_get_wtime();
		simulate_parallel(individual_count, total_epochs, individual_graph, individuals, epoch_statistics);
		time_end = omp_get_wtime() - time_start;
		total_time += time_end;
		assert(assert_epidemic_results(individual_count, epoch_statistics));
		cout << ".";
	}
	cout << (total_time / repeat_count) * 1000.0 << " ms" << endl;

	system("pause");	
}