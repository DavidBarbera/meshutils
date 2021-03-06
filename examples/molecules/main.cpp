////////////////////////////////////////////////////////////////////////////////
//
// (C) Andy Thomason 2016
//
// Example using the basic_mesh class to genererate solvent excluded
// surfaces for molecules.
//


#include <meshutils/mesh.hpp>
#include <meshutils/decoders/pdb_decoder.hpp>
#include <meshutils/encoders/fbx_encoder.hpp>

#include <glm/glm.hpp>

#include <ostream>
#include <fstream>
#include <cmath>
#include <algorithm>
#include <thread>
#include <future>
#include <numeric>


#undef min

inline std::ostream & operator<<(std::ostream &os, const glm::vec3 &v) {
  return os << "vec3(" << v.x << ", " << v.y << ", " << v.z << ")";
}

template <class... P>
const char *fmt(const char *str, P... params) {
  static char buf[256];
  snprintf(buf, sizeof(buf), str, params...);
  return buf;
}

template <class F>
void par_for(int begin, int end, F fn) {
  std::atomic<int> idx;
  idx = begin;
  int num_cpus = std::thread::hardware_concurrency();
  std::vector<std::future<void>> futures(num_cpus);
  for (int cpu = 0; cpu != num_cpus; ++cpu) {
    futures[cpu] = std::async(std::launch::async, [cpu, &idx, end, &fn]() {
      for (;;) {
        int i = idx++;
        printf("[%d %d]", i, cpu);
        fflush(stdout);
        if (i >= end) break;
        fn(i);
      }
    });
  }
  for (int cpu = 0; cpu != num_cpus; ++cpu) {
    futures[cpu].get();
  }
  printf("\n");
}

class molecules {
public:
  molecules(int argc, char **argv) {
    const char *pdb_filename = nullptr;
    const char *output_path = "";
    const char *grid_spacing_text = "0.25";
    bool error = false;
    bool list_chains = false;
    const char *chains = "A-Z";

    for (int i = 1; i != argc; ++i) {
      const char *arg = argv[i];
      if (!strcmp(arg, "--grid-spacing") && i < argc-1) {
        grid_spacing_text = argv[++i];
      } else if (!strcmp(arg, "--chains") && i < argc-1) {
        chains = argv[++i];
      } else if (!strcmp(arg, "-o") && i < argc-1) {
        output_path = argv[++i];
      } else if (!strcmp(arg, "--help")) {
        error = true;
      } else if (!strcmp(arg, "--list-chains")) {
        list_chains = true;
      } else if (arg[0] == '-') {
        printf("invalid argument %s\n", arg);
        error = true;
      } else {
        if (pdb_filename) { printf("only one file will be considered\n"); error = true; }
        pdb_filename = arg;
      }
    }

    float grid_spacing = float(atof(grid_spacing_text));

    if (pdb_filename == nullptr || error) {
      printf(
        "usage: molecule <options> <pdb file name>\n\n"
        "This example generates solvent exclude meshes for a PDB file.\n"
        "You can specify which chains to use with the --chains option.\n"
        "\noptions:\n"
        "--grid-spacing <n>\tgrid spacing (default 0.25) smaller gives more vertices\n"
        "--chains <n>\teg. A-E or ABDEG set of chains to use for generating FBX files. defaults to A-Z...\n"
        "--list-chains <n>\tjust list the chains in the PDB file\n"
        "--output-path <dir>\tdirectory to output files to\n"
        "--help <n>\tshow this text\n"
      ); return; }

    const char *filename = "out";

    //std::ifstream file(CMAKE_SOURCE "/examples/data/2PTC.pdb", std::ios_base::binary);
    std::ifstream file(pdb_filename, std::ios_base::binary);
    std::vector<uint8_t> text;
    if (!file.eof() && !file.fail()) {
      file.seekg(0, std::ios_base::end);
      text.resize((size_t)file.tellg());

      file.seekg(0, std::ios_base::beg);
      file.read((char*)text.data(), text.size());
    } 
  
    meshutils::pdb_decoder pdb(text.data(), text.data() + text.size());

    meshutils::fbx_encoder encoder;
    std::string pdb_chains = pdb.chains();
  
    if (list_chains) {
      printf("chains: %s\n", pdb_chains.c_str());
      return;
    }

    std::vector<glm::vec3> pos;
    std::vector<float> radii;
    std::vector<glm::vec4> colors;

    auto addChain = [&](char chainID) {
      std::vector<glm::vec3> xpos = pdb.pos(chainID);
      std::vector<float> xradii = pdb.radii(chainID);
      std::vector<glm::vec4> xcolors = pdb.colorsByFunction(chainID);
      pos.insert(pos.end(), xpos.begin(), xpos.end());
      radii.insert(radii.end(), xradii.begin(), xradii.end());
      colors.insert(colors.end(), xcolors.begin(), xcolors.end());
    };

    for (const char *p = chains; *p; ++p) {
      if (p[1] == '-' && p[2] && p[2] >= p[1]) {
        for (char i = p[1]; i <= p[2]; ++i) {
          addChain(i);
        }
        p += 2;
      } else {
        addChain(*p);
      }
    }

    struct colored_atom {
      glm::vec4 color;
      glm::vec3 pos;
      float radius;
    };
    std::vector<colored_atom> colored_atoms;

    glm::vec4 white(1, 1, 1, 1);
    for (size_t i = 0; i != colors.size(); ++i) {
      if (colors[i] != white) {
        colored_atom a = { colors[i], pos[i], radii[i] };
        colored_atoms.push_back(a);
      }
    }

    glm::vec3 min = pos[0];
    glm::vec3 max = pos[0];
    for (size_t i = 0; i != pos.size(); ++i) {
      glm::vec3 &p = pos[i];
      min = glm::min(min, p - glm::vec3(radii[i]));
      max = glm::max(max, p + glm::vec3(radii[i]));
    }

    float max_radius = 0;
    for (auto r : radii) {
      max_radius = std::max(max_radius, r);
    }

    float water_radius = 2.0f;
    float recip_gs = 1.0f / grid_spacing;

    min -= water_radius + max_radius;
    max += water_radius + max_radius;

    int xdim = int((max.x - min.x) * recip_gs + 1);
    int ydim = int((max.y - min.y) * recip_gs + 1);
    int zdim = int((max.z - min.z) * recip_gs + 1);
    printf("%d x %d x %d\n", xdim, ydim, zdim);

    auto idx = [xdim, ydim](int x, int y, int z) {
      return ((z * (ydim+1)) + y) * (xdim+1) + x;
    };

    printf("building solvent acessible mesh by inflating the atoms\n");
    std::vector<float> accessible((xdim+1)*(ydim+1)*(zdim+1));
    par_for(0, zdim+1, [&](int z) {
      float zpos = z * grid_spacing + min.z;
      for (int y = 0; y != ydim+1; ++y) {
        float ypos = y * grid_spacing + min.y;
        for (int x = 0; x != xdim+1; ++x) {
          glm::vec3 xyz(x * grid_spacing + min.x, ypos, zpos);
          float value = 1e37f;
          for (size_t i = 0; i != pos.size(); ++i) {
            glm::vec3 p = pos[i];
            float r = radii[i];
            float d2 = glm::dot(xyz - p, xyz - p);
            float r2 = (r + water_radius) * (r + water_radius);
            value = std::min(value, d2 - r2);
          }
          accessible[idx(x, y, z)] = value;
        }
      }
    });

    auto fn = [&accessible, idx](int x, int y, int z) {
      return accessible[idx(x, y, z)];
    };

    auto gen = [&accessible, grid_spacing, min, idx](float x, float y, float z) {
      glm::vec3 xyz(x * grid_spacing + min.x, y * grid_spacing + min.y, z * grid_spacing + min.z);
      return meshutils::pos_mesh::vertex_t(xyz);
    };

    meshutils::pos_mesh amesh(xdim, ydim, zdim, fn, gen);

    std::vector<glm::vec3> zsorter;

    auto &avertices = amesh.vertices();
    for (auto &v : avertices) {
      zsorter.push_back(v.pos());
    }

    auto cmpz = [](const glm::vec3 &a, const glm::vec3 &b) { return a.z < b.z; };
    auto cmpy = [](const glm::vec3 &a, const glm::vec3 &b) { return a.y < b.y; };
    std::sort(zsorter.begin(), zsorter.end(), cmpz);

    printf("building solvent excluded mesh by deflating the acessible mesh\n");
    std::vector<float> excluded((xdim+1)*(ydim+1)*(zdim+1));
    float outside_value = -(water_radius * water_radius);
    par_for(0, zdim+1, [&](int z) {
      std::vector<glm::vec3> ysorter;

      // search only a band of z values in zpos +/- water_radius
      float zpos = z * grid_spacing + min.z;
      auto p = std::lower_bound( zsorter.begin(), zsorter.end(), glm::vec3(0, 0, zpos - (water_radius + grid_spacing)), cmpz);
      auto q = std::upper_bound( zsorter.begin(), zsorter.end(), glm::vec3(0, 0, zpos + (water_radius + grid_spacing)), cmpz);

      for (int y = 0; y != ydim+1; ++y) {
        float ypos = y * grid_spacing + min.y;

        // filter by y position
        ysorter.clear();
        for (auto r = p; r != q; ++r) {
          if (std::abs(r->y - ypos) <= water_radius + grid_spacing) {
            ysorter.push_back(*r);
          }
        }

        for (int x = 0; x != xdim+1; ++x) {
          glm::vec3 xyz(x * grid_spacing + min.x, ypos, zpos);
          float value = 1e37f;
          // only if we are inside the acessible mesh...
          if (accessible[idx(x, y, z)] < 0) {
            // find the closest point on the accessible mesh to xyz.
            for (auto &r : ysorter) {
              float d2 = glm::dot(xyz - r, xyz - r);
              value = std::min(value, d2);
            }
            if (value == 1e37f) {
              value = outside_value;
            } else {
              value -= (water_radius) * (water_radius);
            }
          } else {
            value = outside_value;
          }
          excluded[idx(x, y, z)] = value;
        }
      }
    });

    auto efn = [&excluded, idx](int x, int y, int z) {
      return excluded[idx(x, y, z)];
    };

    auto egen = [&excluded, &colored_atoms, grid_spacing, min, idx](float x, float y, float z) {
      glm::vec3 xyz(x * grid_spacing + min.x, y * grid_spacing + min.y, z * grid_spacing + min.z);
      glm::vec3 normal(1, 0, 0);
      glm::vec2 uv(0, 0);
      glm::vec4 color = glm::vec4(1, 1, 1, 1);
      for (size_t i = 0; i != colored_atoms.size(); ++i) {
        glm::vec3 &pos = colored_atoms[i].pos;
        float r = colored_atoms[i].radius;
        float d2 = glm::dot(xyz - pos, xyz - pos);
        float weight = (r*r*4 - d2) * 1.0f;
        if (weight > 0) {
          weight = std::max(0.0f, std::min(weight, 1.0f));
          color += colored_atoms[i].color * weight;
        }
      }
      color.w = 0;
      color = glm::normalize(color);
      color.w = 1;
      return meshutils::color_mesh::vertex_t(xyz, normal, uv, color);
    };

    meshutils::color_mesh emesh(xdim, ydim, zdim, efn, egen);

    const char *last_slash = pdb_filename;
    const char *last_dot = pdb_filename + strlen(pdb_filename);
    for (const char *p = pdb_filename; *p; ++p) {
      if (*p == '/' || *p == '\\') last_slash = p + 1;
    }
    for (const char *p = last_slash; *p; ++p) {
      if (*p == '.') last_dot = p;
    }

    std::string stem;
    stem.assign(last_slash, last_dot);

    const char *out_filename = fmt("%s_%s_%s.fbx", stem.c_str(), chains, grid_spacing_text);
    printf("writing %s\n", out_filename);
    std::ofstream eof(out_filename, std::ios::binary);
    std::vector<uint8_t> bytes = encoder.saveMesh(emesh);
    eof.write((char*)bytes.data(), bytes.size());
  }
private:
};

int main(int argc, char **argv) {
  molecules m(argc, argv);
}
