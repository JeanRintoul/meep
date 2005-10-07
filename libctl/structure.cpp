#include "meep-ctl.hpp"
#include <ctlgeom.h>

using namespace ctlio;

#define master_printf meep::master_printf
#define MTS material_type_struct

/***********************************************************************/

vector3 vec2vector3(const meep::vec &v)
{
  vector3 v3;
  
  switch (v.dim) {
  case meep::D1:
    v3.x = v.x();
    v3.y = 0;
    v3.z = 0;
    break;
  case meep::D2:
    v3.x = v.x();
    v3.y = v.y();
    v3.z = 0;
    break;
  case meep::D3:
    v3.x = v.x();
    v3.y = v.y();
    v3.z = v.z();
    break;
  case meep::Dcyl:
    v3.x = v.r();
    v3.y = v.z();
    v3.z = 0;
    break;
  }
  return v3;
}

static geom_box gv2box(const meep::geometric_volume &gv)
{
  geom_box box;
  box.low = vec2vector3(gv.get_min_corner());
  box.high = vec2vector3(gv.get_max_corner());
  return box;
}

/***********************************************************************/

class geom_epsilon : public meep::material_function {
  geometric_object_list geometry;
  geom_box_tree geometry_tree;
  geom_box_tree restricted_tree;
  
public:
  geom_epsilon(geometric_object_list g,
	       const meep::geometric_volume &gv);
  virtual ~geom_epsilon();
  
  virtual void set_volume(const meep::geometric_volume &gv);
  virtual void unset_volume(void);
  virtual double eps(const meep::vec &r);
};

geom_epsilon::geom_epsilon(geometric_object_list g,
			   const meep::geometric_volume &gv)
{
  geometry = g; // don't bother making a copy, only used in one place
  
  if (meep::am_master()) {
    for (int i = 0; i < geometry.num_items; ++i) {
      display_geometric_object_info(5, geometry.items[i]);
      
      if (geometry.items[i].material.which_subclass 
	  == MTS::DIELECTRIC)
	printf("%*sdielectric constant epsilon = %g\n",
	       5 + 5, "",
	       geometry.items[i].material.
	       subclass.dielectric_data->epsilon);
    }
  }
  
  geom_fix_objects0(geometry);
  geom_box box = gv2box(gv);
  geometry_tree = create_geom_box_tree0(geometry, box);
  if (verbose && meep::am_master()) {
    printf("Geometric-object bounding-box tree:\n");
    display_geom_box_tree(5, geometry_tree);
    
    int tree_depth, tree_nobjects;
    geom_box_tree_stats(geometry_tree, &tree_depth, &tree_nobjects);
    master_printf("Geometric object tree has depth %d "
		  "and %d object nodes (vs. %d actual objects)\n",
		  tree_depth, tree_nobjects, geometry.num_items);
  }
  
  restricted_tree = geometry_tree;
}

geom_epsilon::~geom_epsilon()
{
  unset_volume();
  destroy_geom_box_tree(geometry_tree);
}

void geom_epsilon::unset_volume(void)
{
  if (restricted_tree != geometry_tree) {
    destroy_geom_box_tree(restricted_tree);
    restricted_tree = geometry_tree;
  }
}

void geom_epsilon::set_volume(const meep::geometric_volume &gv)
{
  unset_volume();
  
  geom_box box = gv2box(gv);
  restricted_tree = create_geom_box_tree0(geometry, box);
}

static material_type eval_material_func(function material_func, vector3 p)
{
  SCM pscm = ctl_convert_vector3_to_scm(p);
  material_type material;
  SCM mo;
  
  mo = gh_call1(material_func, pscm);
  material_type_input(mo, &material);
  
  while (material.which_subclass == MTS::MATERIAL_FUNCTION) {
    material_type m;
    
    mo = gh_call1(material.subclass.
		  material_function_data->material_func,
		  pscm);
    material_type_input(mo, &m);
    material_type_destroy(material);
    material = m;
  }
  
  if (material.which_subclass == MTS::MATERIAL_TYPE_SELF) {
    material_type_copy(&default_material, &material);
  }
  CK(material.which_subclass != MTS::MATERIAL_FUNCTION,
     "infinite loop in material functions");
  
  return material;
}

double geom_epsilon::eps(const meep::vec &r)
{
  double eps = 1.0;
  vector3 p = vec2vector3(r);
  boolean inobject;
  material_type material =
    material_of_point_in_tree_inobject(p, restricted_tree, &inobject);
  
  int destroy_material = 0;
  if (material.which_subclass == MTS::MATERIAL_TYPE_SELF) {
    material = default_material;
  }
  else if (material.which_subclass == MTS::MATERIAL_FUNCTION) {
    material = eval_material_func(
				  material.subclass.
				  material_function_data->material_func,
				  p);
    destroy_material = 1;
  }
  
  switch (material.which_subclass) {
  case MTS::DIELECTRIC:
    eps = material.subclass.dielectric_data->epsilon;
    break;
  case MTS::PERFECT_METAL:
    eps = -meep::infinity;
    break;
  default:
    CK(0, "unknown material type");
  }
  
  if (destroy_material)
    material_type_destroy(material);
  
  return eps;
}

/***********************************************************************/

meep::structure *make_structure(int dims, vector3 size, double resolution,
				geometric_object_list geometry,
				int desired_num_chunks,
				const meep::symmetry &S)
{
  master_printf("-----------\nInitializing structure...\n");
  
  // only cartesian lattices, centered at the origin, are currently allowed
  lattice cart_lattice = {
    {1,0,0},{0,1,0},{0,0,1},
    {0,0,0},
    {1,1,1},
    {1,0,0},{0,1,0},{0,0,1},
    { {1,0,0},{0,1,0},{0,0,1} },
    { {1,0,0},{0,1,0},{0,0,1} }
  };
  vector3 center0 = {0,0,0};
  
  geometry_lattice = cart_lattice;
  geometry_lattice.size = size;
  geometry_center = center0;
  
  number no_size = 2.0 / ctl_get_number("infinity");
  if (size.x <= no_size)
    size.x = 0.0;
  if (size.y <= no_size)
    size.y = 0.0;
  if (size.z <= no_size)
    size.z = 0.0;
  
  if (dims == CYLINDRICAL)
    dimensions = 2;
  else
    dimensions = dims;
  
  master_printf("Working in %d dimensions.\n", dimensions);
  
  meep::volume v;
  switch (dims) {
  case 0: case 1:
    v = meep::vol1d(size.x, resolution);
    break;
  case 2:
    v = meep::vol2d(size.x, size.y, resolution);
    break;
  case 3:
    v = meep::vol3d(size.x, size.y, size.z, resolution);
    break;
  case CYLINDRICAL:
    v = meep::volcyl(size.x, size.y, resolution);
    break;
  default:
    CK(0, "unsupported dimensionality");
  }
  
  geom_epsilon geps(geometry, v.pad().surroundings());
  
  meep::structure *s = new meep::structure(v, geps, meep::no_pml(), S);
  
  master_printf("-----------\n");
  
  return s;
}

/*************************************************************************/
