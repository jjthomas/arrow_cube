#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <algorithm>

#include "fpga_pci.h"
#include "fpga_mgmt.h"
#include "fpga_dma.h"

extern "C" void compute2d_acc(uint8_t **, int, int, uint8_t *, uint32_t *);

#define BLOCK_SIZE 50

void run(int read_fd, int write_fd, pci_bar_handle_t pci_bar_handle, uint8_t *input_buf,
  int input_buf_size, uint8_t *output_buf, int output_buf_size) {
  fpga_dma_burst_write(write_fd, input_buf, input_buf_size, 0);
  fpga_pci_poke(pci_bar_handle, 0x600, 1);
  uint32_t reg_peek;
  do {
    fpga_pci_peek(pci_bar_handle, 0x600, &reg_peek);
    usleep(1000);
  } while (reg_peek != 0);
  fpga_dma_burst_read(read_fd, output_buf, output_buf_size, 1000000000);
}

void compute2d_acc(uint8_t **cols, int num_rows, int num_cols, uint8_t *metric, uint32_t *stats) {
  if (fpga_mgmt_init() != 0) {
    printf("ERROR: fpga_mgmt_init()\n");
  }
  int read_fd = fpga_dma_open_queue(FPGA_DMA_XDMA, 0, 0, true);
  int write_fd = fpga_dma_open_queue(FPGA_DMA_XDMA, 0, 0, false);
  if (read_fd < 0 || write_fd < 0) {
    printf("ERROR: unable to get XDMA read or write handle\n");
  }
  pci_bar_handle_t pci_bar_handle = PCI_BAR_HANDLE_INIT;
  if (fpga_pci_attach(0, 0, 0, 0, &pci_bar_handle) != 0) {
    printf("ERROR: PCI attach\n");
  }

  int input_buf_size = 64 * (num_rows + 1);
  uint8_t *input_buf = (uint8_t *)malloc(input_buf_size);
  *((uint32_t *)input_buf) = num_rows; // store length in first line
  uint8_t *input_data = input_buf + 64;
  int output_buf_size = sizeof(uint32_t) * 512 * BLOCK_SIZE * BLOCK_SIZE;
  uint32_t *output_buf = (uint32_t *)malloc(output_buf_size);
  int num_blocks = (num_cols + BLOCK_SIZE - 1) / BLOCK_SIZE;
  for (int i = 0; i < num_blocks; i++) {
    for (int j = i; j < num_blocks; j++) {
      int i_start_col = i * BLOCK_SIZE;
      int j_start_col = j * BLOCK_SIZE;
      int i_end_col = std::min(num_cols, i_start_col + BLOCK_SIZE);
      int j_end_col = std::min(num_cols, j_start_col + BLOCK_SIZE);
      memset(input_data, 0, 64 * num_rows);
      #pragma omp parallel for
      for (int k = 0; k < num_rows; k++) {
        uint8_t *cur_row = input_data + 64 * k;
        *cur_row = metric[k];
        cur_row += 4; // skip metric
        for (int l = i_start_col; l < i_end_col; l++) {
          int off = l - i_start_col;
          int byte = off >> 1;
          int shift = off & 1 ? 4 : 0;
          cur_row[byte] |= cols[l][k] << shift;
        }
        for (int l = j_start_col; l < j_end_col; l++) {
          int off = l - j_start_col + BLOCK_SIZE;
          int byte = off >> 1;
          int shift = off & 1 ? 4 : 0;
          cur_row[byte] |= cols[l][k] << shift;
        }
      }
      run(read_fd, write_fd, pci_bar_handle, input_buf, input_buf_size,
        (uint8_t *)output_buf, output_buf_size);
      for (int k = i_start_col; k < i_end_col; k++) {
        for (int l = i == j ? k : j_start_col; l < j_end_col; l++) {
          int remaining_triangle_base = num_cols - k;
          // idx in output triangle
          int target_idx = num_cols * (num_cols + 1) / 2 -
            remaining_triangle_base * (remaining_triangle_base + 1) / 2 +
            (l - k);
          int source_idx = (k - i_start_col) * BLOCK_SIZE + (l - j_start_col);
          memcpy(stats + 512 * target_idx, output_buf + 512 * source_idx,
            512 * sizeof(uint32_t));
        }
      }
    }
  }
  free(input_buf);
  free(output_buf);
  close(read_fd);
  close(write_fd);
}