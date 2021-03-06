/*
Highly Optimized Object-oriented Many-particle Dynamics -- Blue Edition
(HOOMD-blue) Open Source Software License Copyright 2009-2014 The Regents of
the University of Michigan All rights reserved.

HOOMD-blue may contain modifications ("Contributions") provided, and to which
copyright is held, by various Contributors who have granted The Regents of the
University of Michigan the right to modify and/or distribute such Contributions.

You may redistribute, use, and create derivate works of HOOMD-blue, in source
and binary forms, provided you abide by the following conditions:

* Redistributions of source code must retain the above copyright notice, this
list of conditions, and the following disclaimer both in the code and
prominently in any materials provided with the distribution.

* Redistributions in binary form must reproduce the above copyright notice, this
list of conditions, and the following disclaimer in the documentation and/or
other materials provided with the distribution.

* All publications and presentations based on HOOMD-blue, including any reports
or published results obtained, in whole or in part, with HOOMD-blue, will
acknowledge its use according to the terms posted at the time of submission on:
http://codeblue.umich.edu/hoomd-blue/citations.html

* Any electronic documents citing HOOMD-Blue will link to the HOOMD-Blue website:
http://codeblue.umich.edu/hoomd-blue/

* Apart from the above required attributions, neither the name of the copyright
holder nor the names of HOOMD-blue's contributors may be used to endorse or
promote products derived from this software without specific prior written
permission.

Disclaimer

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDER AND CONTRIBUTORS ``AS IS'' AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE, AND/OR ANY
WARRANTIES THAT THIS SOFTWARE IS FREE OF INFRINGEMENT ARE DISCLAIMED.

IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

// Maintainer: joaander

/*! \file SFCPackUpdater.cc
    \brief Defines the SFCPackUpdater class
*/

#ifdef WIN32
#pragma warning( push )
#pragma warning( disable : 4103 4244 )
#endif

#include <boost/python.hpp>
using namespace boost::python;
using namespace boost;

#include <math.h>
#include <stdexcept>
#include <algorithm>
#include <fstream>
#include <iostream>

#include "SFCPackUpdater.h"
#include "Communicator.h"

using namespace std;

/*! \param sysdef System to perform sorts on
 */
SFCPackUpdater::SFCPackUpdater(boost::shared_ptr<SystemDefinition> sysdef)
        : Updater(sysdef), m_last_grid(0), m_last_dim(0)
    {
    m_exec_conf->msg->notice(5) << "Constructing SFCPackUpdater" << endl;

    // perform lots of sanity checks
    assert(m_pdata);

    m_sort_order.resize(m_pdata->getMaxN());
    m_particle_bins.resize(m_pdata->getMaxN());

    // set the default grid
    // Grid dimension must always be a power of 2 and determines the memory usage for m_traversal_order
    // To prevent massive overruns of the memory, always use 256 for 3d and 4096 for 2d
    if (m_sysdef->getNDimensions() == 2)
        m_grid = 4096;
    else
        m_grid = 256;

    // register reallocate method with particle data maximum particle number change signal
    m_max_particle_num_change_connection = m_pdata->connectMaxParticleNumberChange(bind(&SFCPackUpdater::reallocate, this));
    }

/*! reallocate the internal arrays
 */
void SFCPackUpdater::reallocate()
    {
    m_sort_order.resize(m_pdata->getMaxN());
    m_particle_bins.resize(m_pdata->getMaxN());
    }

/*! Destructor
 */
SFCPackUpdater::~SFCPackUpdater()
    {
    m_exec_conf->msg->notice(5) << "Destroying SFCPackUpdater" << endl;
    m_max_particle_num_change_connection.disconnect();
    }

/*! Performs the sort.
    \note In an updater list, this sort should be done first, before anyone else
    gets ahold of the particle data

    \param timestep Current timestep of the simulation
 */
void SFCPackUpdater::update(unsigned int timestep)
    {
    m_exec_conf->msg->notice(6) << "SFCPackUpdater: particle sort" << std::endl;

    #ifdef ENABLE_MPI
    /* migrate particles to their respective domains
       this has two consequences:
       1. we do not need to pad the particle bins for particles that are outside the domain
       2. we migrate only, so all ghost particles are cleared, and we can reorder the particle data
          without having to account for ghosts
     */
    if (m_pdata->getDomainDecomposition())
        {
        assert(m_comm);
        m_comm->migrateParticles();
        }
    #endif

    if (m_prof) m_prof->push(m_exec_conf, "SFCPack");

    // figure out the sort order we need to apply
    if (m_sysdef->getNDimensions() == 2)
        getSortedOrder2D();
    else
        getSortedOrder3D();

    // apply that sort order to the particles
    applySortOrder();

    m_pdata->notifyParticleSort();

    if (m_prof) m_prof->pop(m_exec_conf);

    #ifdef ENABLE_MPI
    /* since we migrated, also run exchange ghosts to reestablish ghost particles before some unsuspecting code runs
       after us and assumes that ghosts exist.
    */
    if (m_pdata->getDomainDecomposition())
        {
        assert(m_comm);
        m_comm->exchangeGhosts();
        }
    #endif

    }

void SFCPackUpdater::applySortOrder()
    {
    assert(m_pdata);
    assert(m_sort_order.size() >= m_pdata->getN());
    ArrayHandle<Scalar4> h_pos(m_pdata->getPositions(), access_location::host, access_mode::readwrite);
    ArrayHandle<Scalar4> h_vel(m_pdata->getVelocities(), access_location::host, access_mode::readwrite);
    ArrayHandle<Scalar3> h_accel(m_pdata->getAccelerations(), access_location::host, access_mode::readwrite);
    ArrayHandle<Scalar> h_charge(m_pdata->getCharges(), access_location::host, access_mode::readwrite);
    ArrayHandle<Scalar> h_diameter(m_pdata->getDiameters(), access_location::host, access_mode::readwrite);
    ArrayHandle<int3> h_image(m_pdata->getImages(), access_location::host, access_mode::readwrite);
    ArrayHandle<unsigned int> h_body(m_pdata->getBodies(), access_location::host, access_mode::readwrite);
    ArrayHandle<unsigned int> h_tag(m_pdata->getTags(), access_location::host, access_mode::readwrite);
    ArrayHandle<unsigned int> h_rtag(m_pdata->getRTags(), access_location::host, access_mode::readwrite);

    // construct a temporary holding array for the sorted data
    Scalar4 *scal4_tmp = new Scalar4[m_pdata->getN()];

    // sort positions and types
    for (unsigned int i = 0; i < m_pdata->getN(); i++)
        scal4_tmp[i] = h_pos.data[m_sort_order[i]];
    for (unsigned int i = 0; i < m_pdata->getN(); i++)
        h_pos.data[i] = scal4_tmp[i];

    // sort velocities and mass
    for (unsigned int i = 0; i < m_pdata->getN(); i++)
        scal4_tmp[i] = h_vel.data[m_sort_order[i]];
    for (unsigned int i = 0; i < m_pdata->getN(); i++)
        h_vel.data[i] = scal4_tmp[i];

    Scalar3 *scal3_tmp = new Scalar3[m_pdata->getN()];
    // sort accelerations
    for (unsigned int i = 0; i < m_pdata->getN(); i++)
        scal3_tmp[i] = h_accel.data[m_sort_order[i]];
    for (unsigned int i = 0; i < m_pdata->getN(); i++)
        h_accel.data[i] = scal3_tmp[i];

    Scalar *scal_tmp  = new Scalar[m_pdata->getN()];
    // sort charge
    for (unsigned int i = 0; i < m_pdata->getN(); i++)
        scal_tmp[i] = h_charge.data[m_sort_order[i]];
    for (unsigned int i = 0; i < m_pdata->getN(); i++)
        h_charge.data[i] = scal_tmp[i];

    // sort diameter
    for (unsigned int i = 0; i < m_pdata->getN(); i++)
        scal_tmp[i] = h_diameter.data[m_sort_order[i]];
    for (unsigned int i = 0; i < m_pdata->getN(); i++)
        h_diameter.data[i] = scal_tmp[i];

    // in case anyone access it from frame to frame, sort the net virial
        {
        ArrayHandle<Scalar> h_net_virial(m_pdata->getNetVirial(), access_location::host, access_mode::readwrite);
        unsigned int virial_pitch = m_pdata->getNetVirial().getPitch();

        for (unsigned int j = 0; j < 6; j++)
            {
            for (unsigned int i = 0; i < m_pdata->getN(); i++)
                scal_tmp[i] = h_net_virial.data[j*virial_pitch+m_sort_order[i]];
            for (unsigned int i = 0; i < m_pdata->getN(); i++)
                h_net_virial.data[j*virial_pitch+i] = scal_tmp[i];
            }
        }

    // sort net force, net torque, and orientation
        {
        ArrayHandle<Scalar4> h_net_force(m_pdata->getNetForce(), access_location::host, access_mode::readwrite);

        for (unsigned int i = 0; i < m_pdata->getN(); i++)
            scal4_tmp[i] = h_net_force.data[m_sort_order[i]];
        for (unsigned int i = 0; i < m_pdata->getN(); i++)
            h_net_force.data[i] = scal4_tmp[i];
        }

        {
        ArrayHandle<Scalar4> h_net_torque(m_pdata->getNetTorqueArray(), access_location::host, access_mode::readwrite);

        for (unsigned int i = 0; i < m_pdata->getN(); i++)
            scal4_tmp[i] = h_net_torque.data[m_sort_order[i]];
        for (unsigned int i = 0; i < m_pdata->getN(); i++)
            h_net_torque.data[i] = scal4_tmp[i];
        }

        {
        ArrayHandle<Scalar4> h_orientation(m_pdata->getOrientationArray(), access_location::host, access_mode::readwrite);

        for (unsigned int i = 0; i < m_pdata->getN(); i++)
            scal4_tmp[i] = h_orientation.data[m_sort_order[i]];
        for (unsigned int i = 0; i < m_pdata->getN(); i++)
            h_orientation.data[i] = scal4_tmp[i];
        }

    // sort image
    int3 *int3_tmp = new int3[m_pdata->getN()];
    for (unsigned int i = 0; i < m_pdata->getN(); i++)
        int3_tmp[i] = h_image.data[m_sort_order[i]];
    for (unsigned int i = 0; i < m_pdata->getN(); i++)
        h_image.data[i] = int3_tmp[i];

    // sort body
    unsigned int *uint_tmp = new unsigned int[m_pdata->getN()];
    for (unsigned int i = 0; i < m_pdata->getN(); i++)
        uint_tmp[i] = h_body.data[m_sort_order[i]];
    for (unsigned int i = 0; i < m_pdata->getN(); i++)
        h_body.data[i] = uint_tmp[i];

    // sort global tag
    for (unsigned int i = 0; i < m_pdata->getN(); i++)
        uint_tmp[i] = h_tag.data[m_sort_order[i]];
    for (unsigned int i = 0; i < m_pdata->getN(); i++)
        h_tag.data[i] = uint_tmp[i];

    // rebuild global rtag
    for (unsigned int i = 0; i < m_pdata->getN(); i++)
        {
        h_rtag.data[h_tag.data[i]] = i;
        }

    delete[] scal_tmp;
    delete[] scal4_tmp;
    delete[] scal3_tmp;
    delete[] uint_tmp;
    delete[] int3_tmp;

    }

//! x walking table for the hilbert curve
static int istep[] = {0, 0, 0, 0, 1, 1, 1, 1};
//! y walking table for the hilbert curve
static int jstep[] = {0, 0, 1, 1, 1, 1, 0, 0};
//! z walking table for the hilbert curve
static int kstep[] = {0, 1, 1, 0, 0, 1, 1, 0};


//! Helper function for recursive hilbert curve generation
/*! \param result Output sequence to be permuted by rule 1
    \param in Input sequence
*/
static void permute1(unsigned int result[8], const unsigned int in[8])
    {
    result[0] = in[0];
    result[1] = in[3];
    result[2] = in[4];
    result[3] = in[7];
    result[4] = in[6];
    result[5] = in[5];
    result[6] = in[2];
    result[7] = in[1];
    }

//! Helper function for recursive hilbert curve generation
/*! \param result Output sequence to be permuted by rule 2
    \param in Input sequence
*/
static void permute2(unsigned int result[8], const unsigned int in[8])
    {
    result[0] = in[0];
    result[1] = in[7];
    result[2] = in[6];
    result[3] = in[1];
    result[4] = in[2];
    result[5] = in[5];
    result[6] = in[4];
    result[7] = in[3];
    }

//! Helper function for recursive hilbert curve generation
/*! \param result Output sequence to be permuted by rule 3
    \param in Input sequence
*/
static void permute3(unsigned int result[8], const unsigned int in[8])
    {
    permute2(result, in);
    }

//! Helper function for recursive hilbert curve generation
/*! \param result Output sequence to be permuted by rule 4
    \param in Input sequence
*/
static void permute4(unsigned int result[8], const unsigned int in[8])
    {
    result[0] = in[2];
    result[1] = in[3];
    result[2] = in[0];
    result[3] = in[1];
    result[4] = in[6];
    result[5] = in[7];
    result[6] = in[4];
    result[7] = in[5];
    }

//! Helper function for recursive hilbert curve generation
/*! \param result Output sequence to be permuted by rule 5
    \param in Input sequence
*/
static void permute5(unsigned int result[8], const unsigned int in[8])
    {
    permute4(result, in);
    }

//! Helper function for recursive hilbert curve generation
/*! \param result Output sequence to be permuted by rule 6
    \param in Input sequence
*/
static void permute6(unsigned int result[8], const unsigned int in[8])
    {
    result[0] = in[4];
    result[1] = in[3];
    result[2] = in[2];
    result[3] = in[5];
    result[4] = in[6];
    result[5] = in[1];
    result[6] = in[0];
    result[7] = in[7];
    }

//! Helper function for recursive hilbert curve generation
/*! \param result Output sequence to be permuted by rule 7
    \param in Input sequence
*/
static void permute7(unsigned int result[8], const unsigned int in[8])
    {
    permute6(result, in);
    }

//! Helper function for recursive hilbert curve generation
/*! \param result Output sequence to be permuted by rule 8
    \param in Input sequence
*/
static void permute8(unsigned int result[8], const unsigned int in[8])
    {
    result[0] = in[6];
    result[1] = in[5];
    result[2] = in[2];
    result[3] = in[1];
    result[4] = in[0];
    result[5] = in[3];
    result[6] = in[4];
    result[7] = in[7];
    }

//! Helper function for recursive hilbert curve generation
/*! \param result Output sequence to be permuted by rule \a p-1
    \param in Input sequence
    \param p permutation rule to apply
*/
void permute(unsigned int result[8], const unsigned int in[8], int p)
    {
    switch (p)
        {
        case 0:
            permute1(result, in);
            break;
        case 1:
            permute2(result, in);
            break;
        case 2:
            permute3(result, in);
            break;
        case 3:
            permute4(result, in);
            break;
        case 4:
            permute5(result, in);
            break;
        case 5:
            permute6(result, in);
            break;
        case 6:
            permute7(result, in);
            break;
        case 7:
            permute8(result, in);
            break;
        default:
            assert(false);
        }
    }

//! recursive function for generating hilbert curve traversal order
/*! \param i Current x coordinate in grid
    \param j Current y coordinate in grid
    \param k Current z coordinate in grid
    \param w Number of grid cells wide at the current recursion level
    \param Mx Width of the entire grid (it is cubic, same width in all 3 directions)
    \param cell_order Current permutation order to traverse cells along
    \param traversal_order Traversal order to build up
    \pre \a traversal_order.size() == 0
    \pre Initial call should be with \a i = \a j = \a k = 0, \a w = \a Mx, \a cell_order = (0,1,2,3,4,5,6,7,8)
    \post traversal order contains the grid index (i*Mx*Mx + j*Mx + k) of each grid point
        listed in the order of the hilbert curve
*/
void SFCPackUpdater::generateTraversalOrder(int i, int j, int k, int w, int Mx, unsigned int cell_order[8], vector< unsigned int > &traversal_order)
    {
    if (w == 1)
        {
        // handle base case
        traversal_order.push_back(i*Mx*Mx + j*Mx + k);
        }
    else
        {
        // handle arbitrary case, split the box into 8 sub boxes
        w = w / 2;

        // we ned to handle each sub box in the order defined by cell order
        for (int m = 0; m < 8; m++)
            {
            unsigned int cur_cell = cell_order[m];
            int ic = i + w * istep[cur_cell];
            int jc = j + w * jstep[cur_cell];
            int kc = k + w * kstep[cur_cell];

            unsigned int child_cell_order[8];
            permute(child_cell_order, cell_order, m);
            generateTraversalOrder(ic,jc,kc,w,Mx, child_cell_order, traversal_order);
            }
        }
    }

void SFCPackUpdater::getSortedOrder2D()
    {
    // start by checking the saneness of some member variables
    assert(m_pdata);
    assert(m_sort_order.size() >= m_pdata->getN());

    // make even bin dimensions
    const BoxDim& box = m_pdata->getBox();

    // put the particles in the bins
    {
    ArrayHandle<Scalar4> h_pos(m_pdata->getPositions(), access_location::host, access_mode::read);

    // for each particle
    for (unsigned int n = 0; n < m_pdata->getN(); n++)
        {
        // find the bin each particle belongs in
        Scalar3 p = make_scalar3(h_pos.data[n].x, h_pos.data[n].y, h_pos.data[n].z);
        Scalar3 f = box.makeFraction(p,make_scalar3(0.0,0.0,0.0));
        unsigned int ib = (unsigned int)(f.x * m_grid) % m_grid;
        unsigned int jb = (unsigned int)(f.y * m_grid) % m_grid;

        // record its bin
        unsigned int bin = ib*m_grid + jb;

        m_particle_bins[n] = std::pair<unsigned int, unsigned int>(bin, n);
        }
    }

    // sort the tuples
    sort(m_particle_bins.begin(), m_particle_bins.begin() + m_pdata->getN());

    // translate the sorted order
    for (unsigned int j = 0; j < m_pdata->getN(); j++)
        {
        m_sort_order[j] = m_particle_bins[j].second;
        }
    }

void SFCPackUpdater::getSortedOrder3D()
    {
    // start by checking the saneness of some member variables
    assert(m_pdata);
    assert(m_sort_order.size() >= m_pdata->getN());

    // make even bin dimensions
    const BoxDim& box = m_pdata->getBox();

    // reallocate memory arrays if m_grid changed
    // also regenerate the traversal order
    if (m_last_grid != m_grid || m_last_dim != 3)
        {
        if (m_grid > 256)
            {
            unsigned int mb = m_grid*m_grid*m_grid*4 / 1024 / 1024;
            m_exec_conf->msg->warning() << "sorter is about to allocate a very large amount of memory (" << mb << "MB)"
                 << " and may crash." << endl;
            m_exec_conf->msg->warning() << "            Reduce the amount of memory allocated to prevent this by decreasing the " << endl;
            m_exec_conf->msg->warning() << "            grid dimension (i.e. sorter.set_params(grid=128) ) or by disabling it " << endl;
            m_exec_conf->msg->warning() << "            ( sorter.disable() ) before beginning the run()." << endl;
            }

        // generate the traversal order
        GPUArray<unsigned int> traversal_order(m_grid*m_grid*m_grid,m_exec_conf);
        m_traversal_order.swap(traversal_order);

        vector< unsigned int > reverse_order(m_grid*m_grid*m_grid);
        reverse_order.clear();

        // we need to start the hilbert curve with a seed order 0,1,2,3,4,5,6,7
        unsigned int cell_order[8];
        for (unsigned int i = 0; i < 8; i++)
            cell_order[i] = i;
        generateTraversalOrder(0,0,0, m_grid, m_grid, cell_order, reverse_order);

        // access traversal order
        ArrayHandle<unsigned int> h_traversal_order(m_traversal_order, access_location::host, access_mode::overwrite);

        for (unsigned int i = 0; i < m_grid*m_grid*m_grid; i++)
            h_traversal_order.data[reverse_order[i]] = i;

        // write the traversal order out to a file for testing/presentations
        // writeTraversalOrder("hilbert.mol2", reverse_order);

        m_last_grid = m_grid;
        // store the last system dimension computed so we can be mindful if that ever changes
        m_last_dim = m_sysdef->getNDimensions();
        }

    // sanity checks
    assert(m_particle_bins.size() >= m_pdata->getN());
    assert(m_traversal_order.getNumElements() == m_grid*m_grid*m_grid);

    // put the particles in the bins
    ArrayHandle<Scalar4> h_pos(m_pdata->getPositions(), access_location::host, access_mode::read);

    // access traversal order
    ArrayHandle<unsigned int> h_traversal_order(m_traversal_order, access_location::host, access_mode::read);

    // for each particle
    for (unsigned int n = 0; n < m_pdata->getN(); n++)
        {
        Scalar3 p = make_scalar3(h_pos.data[n].x, h_pos.data[n].y, h_pos.data[n].z);
        Scalar3 f = box.makeFraction(p,make_scalar3(0.0,0.0,0.0));
        unsigned int ib = (unsigned int)(f.x * m_grid) % m_grid;
        unsigned int jb = (unsigned int)(f.y * m_grid) % m_grid;
        unsigned int kb = (unsigned int)(f.z * m_grid) % m_grid;

        // record its bin
        unsigned int bin = ib*(m_grid*m_grid) + jb * m_grid + kb;

        m_particle_bins[n] = std::pair<unsigned int, unsigned int>(h_traversal_order.data[bin], n);
        }

    // sort the tuples
    sort(m_particle_bins.begin(), m_particle_bins.begin() + m_pdata->getN());

    // translate the sorted order
    for (unsigned int j = 0; j < m_pdata->getN(); j++)
        {
        m_sort_order[j] = m_particle_bins[j].second;
        }
    }

void SFCPackUpdater::writeTraversalOrder(const std::string& fname, const vector< unsigned int >& reverse_order)
    {
    m_exec_conf->msg->notice(2) << "sorter: Writing space filling curve traversal order to " << fname << endl;
    ofstream f(fname.c_str());
    f << "@<TRIPOS>MOLECULE" <<endl;
    f << "Generated by HOOMD" << endl;
    f << m_traversal_order.getNumElements() << " " << m_traversal_order.getNumElements()-1 << endl;
    f << "NO_CHARGES" << endl;

    f << "@<TRIPOS>ATOM" << endl;
    m_exec_conf->msg->notice(2) << "sorter: Writing " << m_grid << "^3 grid cells" << endl;

    for (unsigned int i=0; i < reverse_order.size(); i++)
        {
        unsigned int idx = reverse_order[i];
        unsigned int ib = idx / (m_grid * m_grid);
        unsigned int jb = (idx - ib*m_grid*m_grid) / m_grid;
        unsigned int kb = (idx - ib*m_grid*m_grid - jb*m_grid);

        f << i+1 << " B " << ib << " " << jb << " "<< kb << " " << "B" << endl;
        idx++;
        }

    f << "@<TRIPOS>BOND" << endl;
    for (unsigned int i = 0; i < m_traversal_order.getNumElements()-1; i++)
        {
        f << i+1 << " " << i+1 << " " << i+2 << " 1" << endl;
        }
    }

void export_SFCPackUpdater()
    {
    class_<SFCPackUpdater, boost::shared_ptr<SFCPackUpdater>, bases<Updater>, boost::noncopyable>
    ("SFCPackUpdater", init< boost::shared_ptr<SystemDefinition> >())
    .def("setGrid", &SFCPackUpdater::setGrid)
    ;
    }

#ifdef WIN32
#pragma warning( pop )
#endif
