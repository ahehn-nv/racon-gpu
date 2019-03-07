/*!
 * @file cudabatch.cpp
 *
 * @brief CUDABatch class source file
 */

#include <string>
#include <iostream>
#include <cstring>

#include "cudapoa_kernels.cuh"

#include "cudabatch.hpp"
#include "cudautils.hpp"

#ifndef TABS
#define TABS printTabs(bid_)
#endif

namespace racon {

uint32_t CUDABatchProcessor::batches = 0;

inline std::string printTabs(uint32_t tab_count)
{
    std::string s;
    for(uint32_t i = 0; i < tab_count; i++)
    {
        s += "\t";
    }
    return s;
}

std::unique_ptr<CUDABatchProcessor> createCUDABatch(uint32_t max_windows, uint32_t max_window_depth)
{
    return std::unique_ptr<CUDABatchProcessor>(new CUDABatchProcessor(max_windows, max_window_depth));
}

CUDABatchProcessor::CUDABatchProcessor(uint32_t max_windows, uint32_t max_window_depth)
    : max_windows_(max_windows)
    , max_depth_per_window_(max_window_depth)
    , windows_()
    , sequence_count_()
    , stream_()
    , consensus_pitch_()
{
    bid_ = CUDABatchProcessor::batches++;

    // Create new CUDA stream.
    cudaStreamCreate(&stream_);

    // Allocate host memory and CUDA memory based on max sequence and target counts.

    // Input buffers.
    uint32_t input_size = max_windows_ * max_depth_per_window_ * CUDAPOA_MAX_SEQUENCE_SIZE;
    cudaHostAlloc((void**) &inputs_h_, input_size * sizeof(uint8_t),
                  cudaHostAllocDefault);
    cudaHostAlloc((void**) &num_sequences_per_window_h_, max_windows * sizeof(uint16_t),
            cudaHostAllocDefault);
    cudaHostAlloc((void**) &sequence_lengths_h_, max_windows * max_depth_per_window_* sizeof(uint16_t),
            cudaHostAllocDefault);

    cudaMallocPitch((void**) &inputs_d_,
                    &input_pitch_,
                    sizeof(uint8_t) * CUDAPOA_MAX_SEQUENCE_SIZE,
                    max_windows_ * max_depth_per_window_);

    input_size = max_windows_ * max_depth_per_window_ * input_pitch_;
    cudaMalloc((void**)&num_sequences_per_window_d_, max_windows * sizeof(uint16_t));
    input_size += max_windows * sizeof(uint16_t);
    cudaMalloc((void**)&sequence_lengths_d_, max_windows * max_depth_per_window_ * sizeof(uint16_t));
    input_size += max_windows * max_depth_per_window_ * sizeof(uint16_t);

    cudaCheckError(TABS.append(std::to_string(bid_)).append(std::string(" Could not allocate input memory.")));
    std::cout << TABS << bid_ << " Allocated input buffers of size " << (static_cast<float>(input_size)  / (1024 * 1024)) << "MB" << std::endl;

    // Output buffers.
    input_size = max_windows_ * CUDAPOA_MAX_SEQUENCE_SIZE;
    cudaHostAlloc((void**) &consensus_h_, input_size * sizeof(uint8_t),
                  cudaHostAllocDefault);

    input_size = max_windows_ * consensus_pitch_;
    cudaMallocPitch((void**) &consensus_d_,
                    &consensus_pitch_,
                    sizeof(uint8_t) * CUDAPOA_MAX_SEQUENCE_SIZE,
                    max_windows_);
    cudaCheckError(TABS.append(std::to_string(bid_)).append(std::string(" Could not allocate output memory.")));
    std::cout << TABS << bid_ << " Allocated output buffers of size " << (static_cast<float>(input_size)  / (1024 * 1024)) << "MB" << std::endl;

    // Buffers for storing NW scores and backtrace.
    cudaMalloc((void**) &scores_d_, sizeof(int32_t) * CUDAPOA_MAX_MATRIX_DIMENSION * CUDAPOA_MAX_MATRIX_DIMENSION * NUM_BLOCKS * NUM_THREADS);
    cudaMalloc((void**) &traceback_i_d_, sizeof(int16_t) * CUDAPOA_MAX_MATRIX_DIMENSION * NUM_BLOCKS * NUM_THREADS);
    cudaMalloc((void**) &traceback_j_d_, sizeof(int16_t) * CUDAPOA_MAX_MATRIX_DIMENSION * NUM_BLOCKS * NUM_THREADS);

    cudaCheckError(TABS.append(std::to_string(bid_)).append(std::string(" Could not allocate temp memory.")));

    // Debug print for size allocated.
    uint32_t temp_size = (sizeof(int32_t) * CUDAPOA_MAX_MATRIX_DIMENSION * CUDAPOA_MAX_MATRIX_DIMENSION * NUM_BLOCKS * NUM_THREADS);
    temp_size += 2 * (sizeof(int16_t) * CUDAPOA_MAX_MATRIX_DIMENSION * NUM_BLOCKS * NUM_THREADS);
    std::cout << TABS << bid_ << " Allocated temp buffers of size " << (static_cast<float>(temp_size)  / (1024 * 1024)) << "MB" << std::endl;

    // Allocate graph buffers.
    cudaMalloc((void**) &nodes_d_, sizeof(uint8_t) * CUDAPOA_MAX_NODES_PER_WINDOW * NUM_BLOCKS * NUM_THREADS);
    cudaMalloc((void**) &node_alignments_d_, sizeof(uint16_t) * CUDAPOA_MAX_NODES_PER_WINDOW * CUDAPOA_MAX_NODE_ALIGNMENTS * NUM_BLOCKS * NUM_THREADS);
    cudaMalloc((void**) &node_alignment_count_d_, sizeof(uint16_t) * CUDAPOA_MAX_NODES_PER_WINDOW * NUM_BLOCKS * NUM_THREADS);
    cudaMalloc((void**) &incoming_edges_d_, sizeof(uint16_t) * CUDAPOA_MAX_NODES_PER_WINDOW * CUDAPOA_MAX_NODE_EDGES * NUM_BLOCKS * NUM_THREADS);
    cudaMalloc((void**) &incoming_edge_count_d_, sizeof(uint16_t) * CUDAPOA_MAX_NODES_PER_WINDOW * NUM_BLOCKS * NUM_THREADS);
    cudaMalloc((void**) &outgoing_edges_d_, sizeof(uint16_t) * CUDAPOA_MAX_NODES_PER_WINDOW * CUDAPOA_MAX_NODE_EDGES * NUM_BLOCKS * NUM_THREADS);
    cudaMalloc((void**) &outgoing_edge_count_d_, sizeof(uint16_t) * CUDAPOA_MAX_NODES_PER_WINDOW * NUM_BLOCKS * NUM_THREADS);
    cudaMalloc((void**) &incoming_edges_weights_d_, sizeof(uint16_t) * CUDAPOA_MAX_NODES_PER_WINDOW * CUDAPOA_MAX_NODE_EDGES * NUM_BLOCKS * NUM_THREADS);
    cudaMalloc((void**) &outoing_edges_weights_d_, sizeof(uint16_t) * CUDAPOA_MAX_NODES_PER_WINDOW * CUDAPOA_MAX_NODE_EDGES * NUM_BLOCKS * NUM_THREADS);
    cudaMalloc((void**) &sorted_poa_d_, sizeof(uint16_t) * CUDAPOA_MAX_NODES_PER_WINDOW * NUM_BLOCKS * NUM_THREADS);
    cudaMalloc((void**) &sorted_poa_node_map_d_, sizeof(uint16_t) * CUDAPOA_MAX_NODES_PER_WINDOW * NUM_BLOCKS * NUM_THREADS);

    cudaMemset(nodes_d_, 0, sizeof(uint8_t) * CUDAPOA_MAX_NODES_PER_WINDOW * NUM_BLOCKS * NUM_THREADS);
    cudaMemset(node_alignments_d_, 0, sizeof(uint16_t) * CUDAPOA_MAX_NODES_PER_WINDOW * CUDAPOA_MAX_NODE_ALIGNMENTS * NUM_BLOCKS * NUM_THREADS);
    cudaMemset(node_alignment_count_d_, 0, sizeof(uint16_t) * CUDAPOA_MAX_NODES_PER_WINDOW * NUM_BLOCKS * NUM_THREADS);
    cudaMemset(incoming_edges_d_,0,  sizeof(uint16_t) * CUDAPOA_MAX_NODES_PER_WINDOW * CUDAPOA_MAX_NODE_EDGES * NUM_BLOCKS * NUM_THREADS);
    cudaMemset(incoming_edge_count_d_,0,  sizeof(uint16_t) * CUDAPOA_MAX_NODES_PER_WINDOW * NUM_BLOCKS * NUM_THREADS);
    cudaMemset(outgoing_edges_d_, 0, sizeof(uint16_t) * CUDAPOA_MAX_NODES_PER_WINDOW * CUDAPOA_MAX_NODE_EDGES * NUM_BLOCKS * NUM_THREADS);
    cudaMemset(outgoing_edge_count_d_, 0, sizeof(uint16_t) * CUDAPOA_MAX_NODES_PER_WINDOW * NUM_BLOCKS * NUM_THREADS);
    cudaMemset(incoming_edges_weights_d_, 0, sizeof(uint16_t) * CUDAPOA_MAX_NODES_PER_WINDOW * CUDAPOA_MAX_NODE_EDGES * NUM_BLOCKS * NUM_THREADS);
    cudaMemset(outoing_edges_weights_d_, 0, sizeof(uint16_t) * CUDAPOA_MAX_NODES_PER_WINDOW * CUDAPOA_MAX_NODE_EDGES * NUM_BLOCKS * NUM_THREADS);
    cudaMemset(sorted_poa_d_, 0, sizeof(uint16_t) * CUDAPOA_MAX_NODES_PER_WINDOW * NUM_BLOCKS * NUM_THREADS);

    cudaCheckError(TABS.append(std::to_string(bid_)).append(std::string(" Could not allocate temp memory2.")));

    // Debug print for size allocated.
    temp_size = sizeof(uint8_t) * CUDAPOA_MAX_NODES_PER_WINDOW * NUM_BLOCKS * NUM_THREADS;
    temp_size += sizeof(uint16_t) * CUDAPOA_MAX_NODES_PER_WINDOW * CUDAPOA_MAX_NODE_ALIGNMENTS * NUM_BLOCKS * NUM_THREADS;
    temp_size += sizeof(uint16_t) * CUDAPOA_MAX_NODES_PER_WINDOW * NUM_BLOCKS * NUM_THREADS;
    temp_size += sizeof(uint16_t) * CUDAPOA_MAX_NODES_PER_WINDOW * CUDAPOA_MAX_NODE_EDGES * NUM_BLOCKS * NUM_THREADS;
    temp_size += sizeof(uint16_t) * CUDAPOA_MAX_NODES_PER_WINDOW * NUM_BLOCKS * NUM_THREADS;
    temp_size += sizeof(uint16_t) * CUDAPOA_MAX_NODES_PER_WINDOW * CUDAPOA_MAX_NODE_EDGES * NUM_BLOCKS * NUM_THREADS;
    temp_size += sizeof(uint16_t) * CUDAPOA_MAX_NODES_PER_WINDOW * NUM_BLOCKS * NUM_THREADS;
    temp_size += sizeof(uint16_t) * CUDAPOA_MAX_NODES_PER_WINDOW * CUDAPOA_MAX_NODE_EDGES * NUM_BLOCKS * NUM_THREADS;
    temp_size += sizeof(uint16_t) * CUDAPOA_MAX_NODES_PER_WINDOW * CUDAPOA_MAX_NODE_EDGES * NUM_BLOCKS * NUM_THREADS;
    temp_size += sizeof(uint16_t) * CUDAPOA_MAX_NODES_PER_WINDOW * NUM_BLOCKS * NUM_THREADS;
    std::cout << TABS << bid_ << " Allocated temp buffers of size " << (static_cast<float>(temp_size)  / (1024 * 1024)) << "MB" << std::endl;
}

CUDABatchProcessor::~CUDABatchProcessor()
{
    // Destroy CUDA stream.
    cudaStreamDestroy(stream_);
    cudaCheckError(TABS.append(std::to_string(bid_)).append(std::string(" Could not destroy stream.")));

    // Free all the host and CUDA memory.
    cudaFree(consensus_d_);

    cudaFree(scores_d_);
    cudaFree(traceback_i_d_);
    cudaFree(traceback_j_d_);

    std::cout << TABS << "Destroyed buffers." << std::endl;

    cudaFree(nodes_d_);
    cudaFree(node_alignments_d_);
    cudaFree(node_alignment_count_d_);
    cudaFree(incoming_edges_d_);
    cudaFree(incoming_edge_count_d_);
    cudaFree(outgoing_edges_d_);
    cudaFree(outgoing_edge_count_d_);
    cudaFree(incoming_edges_weights_d_);
    cudaFree(outoing_edges_weights_d_);
    cudaFree(sorted_poa_d_);
}

bool CUDABatchProcessor::doesWindowFit(std::shared_ptr<Window> window) const
{
    if (window->sequences_.size() > max_depth_per_window_)
    {
        std::cerr << TABS << bid_ << " Number of sequences in window is greater than max depth!\n";
    }
    // Checks if adding new window will go over either the MAX_SEQUENCES
    // or max_windows_ count of the batch.
    return (windows_.size() + 1 <= max_windows_);
}

bool CUDABatchProcessor::addWindow(std::shared_ptr<Window> window)
{
    if (doesWindowFit(window))
    {
        windows_.push_back(window);
        sequence_count_ += std::min(max_depth_per_window_, (uint32_t) window->sequences_.size());
        return true;
    }
    else
    {
        return false;
    }
}

bool CUDABatchProcessor::hasWindows() const
{
    return (windows_.size() != 0);
}

void CUDABatchProcessor::generateMemoryMap()
{
    // Fill host/cuda memory with sequence information.
    for(uint32_t i = 0; i < windows_.size(); i++)
    {
        auto window = windows_.at(i);
        uint32_t input_window_offset = i * max_depth_per_window_;
        uint16_t num_seqs = 0;
        for(uint32_t j = 0; j < std::min(max_depth_per_window_, (uint32_t) window->sequences_.size()); j++)
        {
            uint32_t input_sequence_offset = input_window_offset + j;
            auto seq = window->sequences_.at(j);

            if (seq.second > CUDAPOA_MAX_SEQUENCE_SIZE)
            {
                std::cerr << TABS << bid_ 
                    << " sequence size " << seq.second
                    << " larger than max size of " << CUDAPOA_MAX_SEQUENCE_SIZE
                    << std::endl;
                exit(-1);
            }

            memcpy(&(inputs_h_[input_sequence_offset * CUDAPOA_MAX_SEQUENCE_SIZE]),
                   seq.first,
                   seq.second);

            num_seqs++;
            sequence_lengths_h_[i * max_depth_per_window_ + j] = seq.second;
        }
        num_sequences_per_window_h_[i] = num_seqs;
        std::cout << "Sequences is " << std::min(max_depth_per_window_, (uint32_t) window->sequences_.size()) << std::endl;
    }

    std::cout << TABS << bid_ << " Launching data copy" << std::endl;
    cudaMemcpy2DAsync(inputs_d_, input_pitch_,
            inputs_h_.get(), CUDAPOA_MAX_SEQUENCE_SIZE,
            CUDAPOA_MAX_SEQUENCE_SIZE, max_windows_ * max_depth_per_window_,
            cudaMemcpyHostToDevice, stream_);
    cudaMemcpyAsync(num_sequences_per_window_d_, num_sequences_per_window_h_,
            max_windows_ * sizeof(uint16_t), cudaMemcpyHostToDevice, stream_);
    cudaMemcpyAsync(sequence_lengths_d_, sequence_lengths_h_,
            max_depth_per_window_ * max_windows_ * sizeof(uint16_t), cudaMemcpyHostToDevice, stream_);

    cudaCheckError(TABS.append(std::to_string(bid_)).append(std::string(" Could not copy window data to device.")));
    std::cout << TABS << bid_ << " Launched data copy" << std::endl;
}

void CUDABatchProcessor::generatePOA()
{
    // Launch kernel to run 1 POA per thread in thread block.
    std::cout << TABS << bid_ << " Launching kernel for " << windows_.size() << std::endl;
    nvidia::cudapoa::generatePOA(consensus_d_,
                                 consensus_pitch_,
                                 inputs_d_,
                                 input_pitch_,
                                 CUDAPOA_MAX_SEQUENCE_SIZE,
                                 num_sequences_per_window_d_,
                                 sequence_lengths_d_,
                                 max_depth_per_window_,
                                 windows_.size(),
                                 NUM_THREADS,
                                 NUM_BLOCKS,
                                 stream_,
                                 scores_d_,
                                 traceback_i_d_,
                                 traceback_j_d_,
                                 nodes_d_,
                                 incoming_edges_d_,
                                 incoming_edge_count_d_,
                                 outgoing_edges_d_,
                                 outgoing_edge_count_d_,
                                 incoming_edges_weights_d_,
                                 outoing_edges_weights_d_,
                                 sorted_poa_d_,
                                 sorted_poa_node_map_d_,
                                 node_alignments_d_,
                                 node_alignment_count_d_);
    cudaCheckError(TABS.append(std::to_string(bid_)).append(std::string(" Could not launch kernel properly.")));
    std::cout << TABS << bid_ << " Launched kernel" << std::endl;
}

void CUDABatchProcessor::getConsensus()
{
    std::cout << TABS << bid_ << " Launching memcpy D2H" << std::endl;
    cudaMemcpy2DAsync(consensus_h_.get(),
                      CUDAPOA_MAX_SEQUENCE_SIZE,
                      consensus_d_,
                      consensus_pitch_,
                      CUDAPOA_MAX_SEQUENCE_SIZE,
                      max_windows_,
                      cudaMemcpyDeviceToHost,
                      stream_);
    cudaStreamSynchronize(stream_);

    cudaCheckError(TABS.append(std::to_string(bid_)).append(std::string(" Could not copy over output from device.")));
    std::cout << TABS << bid_ << " Finished memcpy D2H" << std::endl;

    for(uint32_t i = 0; i < windows_.size(); i++)
    {
        char* c = reinterpret_cast<char *>(&consensus_h_[i * CUDAPOA_MAX_SEQUENCE_SIZE]);
        windows_.at(i)->consensus_ = std::string(c);
    }
}

bool CUDABatchProcessor::generateConsensus()
{
    // Generate consensus for all windows in the batch
    generateMemoryMap();
    generatePOA();
    getConsensus();

    return true;
}

void CUDABatchProcessor::reset()
{
    windows_.clear();
    sequence_count_ = 0;

    // Clear host and device memory.
    memset(&inputs_h_[0], 0, max_windows_ * max_depth_per_window_ * CUDAPOA_MAX_SEQUENCE_SIZE);
    cudaMemsetAsync(inputs_d_, 0, max_windows_ * max_depth_per_window_ * input_pitch_, stream_);
    cudaCheckError(TABS.append(std::to_string(bid_)).append( std::string(" Could not reset memory.")));
}

} // namespace racon
