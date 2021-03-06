#include "Flow.hpp"
#include "Print.hpp"
#include <cmath>
#include <assert.h>

namespace Slic3r {

// This static method returns a sane extrusion width default.
static inline float auto_extrusion_width(FlowRole role, float nozzle_diameter, float height)
{
#if 0
    // Here we calculate a sane default by matching the flow speed (at the nozzle) and the feed rate.
    // shape: rectangle with semicircles at the ends
    // This "sane" extrusion width gives the following results for a 0.4mm dmr nozzle:
    // Layer   Calculated  Calculated width 
    // heigh   extrusion   over nozzle
    //         width       diameter
    // 0.40    0.40        1.00
    // 0.35    0.43        1.09
    // 0.30    0.48        1.21
    // 0.25    0.56        1.39
    // 0.20    0.67        1.68
    // 0.15    0.87        2.17
    // 0.10    1.28        3.20
    // 0.05    2.52        6.31
    //
    float width = float(0.25 * (nozzle_diameter * nozzle_diameter) * PI / height + height * (1.0 - 0.25 * PI));

    switch (role) {
    case frExternalPerimeter:
    case frSupportMaterial:
    case frSupportMaterialInterface:
        return nozzle_diameter;
    case frPerimeter:
    case frSolidInfill:
    case frTopSolidInfill:
        // do not limit width for sparse infill so that we use full native flow for it
        return std::min(std::max(width, nozzle_diameter * 1.05f), nozzle_diameter * 1.7f);
    case frInfill:
    default:
        return std::max(width, nozzle_diameter * 1.05f);
    }
#else
    switch (role) {
    case frSupportMaterial:
    case frSupportMaterialInterface:
    case frTopSolidInfill:
        return nozzle_diameter;
    default:
    case frExternalPerimeter:
    case frPerimeter:
    case frSolidInfill:
    case frInfill:
        return 1.125f * nozzle_diameter;
    }
#endif
}

// This constructor builds a Flow object from an extrusion width config setting
// and other context properties.
Flow Flow::new_from_config_width(FlowRole role, const ConfigOptionFloatOrPercent &width, float nozzle_diameter, float height, float bridge_flow_ratio)
{
    // we need layer height unless it's a bridge
    if (height <= 0 && bridge_flow_ratio == 0) 
        CONFESS("Invalid flow height supplied to new_from_config_width()");

    float w;
    if (bridge_flow_ratio > 0) {
        // If bridge flow was requested, calculate the bridge width.
        height = w = (bridge_flow_ratio == 1.) ?
            // optimization to avoid sqrt()
            nozzle_diameter :
            sqrt(bridge_flow_ratio) * nozzle_diameter;
    } else if (! width.percent && width.value == 0.) {
        // If user left option to 0, calculate a sane default width.
        w = auto_extrusion_width(role, nozzle_diameter, height);
    } else {
        // If user set a manual value, use it.
        w = float(width.get_abs_value(height));
    }
    
    return Flow(w, height, nozzle_diameter, bridge_flow_ratio > 0);
}

// This constructor builds a Flow object from a given centerline spacing.
Flow Flow::new_from_spacing(float spacing, float nozzle_diameter, float height, bool bridge) 
{
    // we need layer height unless it's a bridge
    if (height <= 0 && !bridge) 
        CONFESS("Invalid flow height supplied to new_from_spacing()");
    // Calculate width from spacing.
    // For normal extrusons, extrusion width is wider than the spacing due to the rounding and squishing of the extrusions.
    // For bridge extrusions, the extrusions are placed with a tiny BRIDGE_EXTRA_SPACING gaps between the threads.
    float width = float(bridge ?
        (spacing - BRIDGE_EXTRA_SPACING) : 
#ifdef HAS_PERIMETER_LINE_OVERLAP
        (spacing + PERIMETER_LINE_OVERLAP_FACTOR * height * (1. - 0.25 * PI));
#else
        (spacing + height * (1. - 0.25 * PI)));
#endif
    return Flow(width, bridge ? width : height, nozzle_diameter, bridge);
}

// This method returns the centerline spacing between two adjacent extrusions 
// having the same extrusion width (and other properties).
float Flow::spacing() const 
{
#ifdef HAS_PERIMETER_LINE_OVERLAP
    if (this->bridge)
        return this->width + BRIDGE_EXTRA_SPACING;
    // rectangle with semicircles at the ends
    float min_flow_spacing = this->width - this->height * (1. - 0.25 * PI);
    return this->width - PERIMETER_LINE_OVERLAP_FACTOR * (this->width - min_flow_spacing);
#else
    return float(this->bridge ? (this->width + BRIDGE_EXTRA_SPACING) : (this->width - this->height * (1. - 0.25 * PI)));
#endif
}

// This method returns the centerline spacing between an extrusion using this
// flow and another one using another flow.
// this->spacing(other) shall return the same value as other.spacing(*this)
float Flow::spacing(const Flow &other) const
{
    assert(this->height == other.height);
    assert(this->bridge == other.bridge);
    return float(this->bridge ? 
        0.5 * this->width + 0.5 * other.width + BRIDGE_EXTRA_SPACING :
        0.5 * this->spacing() + 0.5 * other.spacing());
}

// This method returns extrusion volume per head move unit.
double Flow::mm3_per_mm() const 
{
    return this->bridge ?
        (this->width * this->width) * 0.25 * PI :
        this->width * this->height + 0.25 * (this->height * this->height) / (PI - 4.0);
}

Flow support_material_flow(const PrintObject *object, float layer_height)
{
    return Flow::new_from_config_width(
        frSupportMaterial,
        // The width parameter accepted by new_from_config_width is of type ConfigOptionFloatOrPercent, the Flow class takes care of the percent to value substitution.
        (object->config.support_material_extrusion_width.value > 0) ? object->config.support_material_extrusion_width : object->config.extrusion_width,
        // if object->config.support_material_extruder == 0 (which means to not trigger tool change, but use the current extruder instead), get_at will return the 0th component.
        float(object->print()->config.nozzle_diameter.get_at(object->config.support_material_extruder-1)),
        (layer_height > 0.f) ? layer_height : float(object->config.layer_height.value),
        false);
}

Flow support_material_1st_layer_flow(const PrintObject *object, float layer_height)
{
    return Flow::new_from_config_width(
        frSupportMaterial,
        // The width parameter accepted by new_from_config_width is of type ConfigOptionFloatOrPercent, the Flow class takes care of the percent to value substitution.
        (object->print()->config.first_layer_extrusion_width.value > 0) ? object->print()->config.first_layer_extrusion_width : object->config.support_material_extrusion_width,
        float(object->print()->config.nozzle_diameter.get_at(object->config.support_material_extruder-1)),
        (layer_height > 0.f) ? layer_height : float(object->config.first_layer_height.get_abs_value(object->config.layer_height.value)),
        false);
}

Flow support_material_interface_flow(const PrintObject *object, float layer_height)
{
    return Flow::new_from_config_width(
        frSupportMaterialInterface,
        // The width parameter accepted by new_from_config_width is of type ConfigOptionFloatOrPercent, the Flow class takes care of the percent to value substitution.
        (object->config.support_material_extrusion_width > 0) ? object->config.support_material_extrusion_width : object->config.extrusion_width,
        // if object->config.support_material_interface_extruder == 0 (which means to not trigger tool change, but use the current extruder instead), get_at will return the 0th component.
        float(object->print()->config.nozzle_diameter.get_at(object->config.support_material_interface_extruder-1)),
        (layer_height > 0.f) ? layer_height : float(object->config.layer_height.value),
        false);
}

}
