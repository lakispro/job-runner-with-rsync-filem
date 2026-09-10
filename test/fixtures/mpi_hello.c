/* A real MPI program, not just "hostname": the point is to exercise PMIx
 * wire-up and a collective across the DVM, which is what actually proves
 * the daemons agreed on who they are. */
#include <mpi.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
    int rank, size, len, sum, expected;
    char host[MPI_MAX_PROCESSOR_NAME];

    MPI_Init(&argc, &argv);
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &size);
    MPI_Get_processor_name(host, &len);

    printf("rank %d/%d on %s\n", rank, size, host);
    fflush(stdout);

    /* every rank contributes its rank; the sum is only right if all of
     * them are really in the same communicator */
    MPI_Allreduce(&rank, &sum, 1, MPI_INT, MPI_SUM, MPI_COMM_WORLD);
    MPI_Barrier(MPI_COMM_WORLD);

    expected = size * (size - 1) / 2;
    if (0 == rank) {
        printf("allreduce %s: got %d, expected %d\n",
               (sum == expected) ? "OK" : "WRONG", sum, expected);
    }
    MPI_Finalize();
    return 0;
}
