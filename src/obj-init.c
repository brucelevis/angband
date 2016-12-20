/**
 * \file obj-init.c
 * \brief Various game initialization routines
 *
 * Copyright (c) 1997 Ben Harrison
 *
 * This work is free software; you can redistribute it and/or modify it
 * under the terms of either:
 *
 * a) the GNU General Public License as published by the Free Software
 *    Foundation, version 2, or
 *
 * b) the "Angband licence":
 *    This software may be copied and distributed for educational, research,
 *    and not for profit purposes provided that this copyright and statement
 *    are included in all such copies.  Other copyrights may also apply.
 *
 * This file is used to initialize various variables and arrays for objects
 * in the Angband game.
 *
 * Several of the arrays for Angband are built from data files in the
 * "lib/gamedata" directory.
 */


#include "angband.h"
#include "buildid.h"
#include "effects.h"
#include "init.h"
#include "mon-util.h"
#include "obj-curse.h"
#include "obj-ignore.h"
#include "obj-list.h"
#include "obj-make.h"
#include "obj-pile.h"
#include "obj-randart.h"
#include "obj-slays.h"
#include "obj-tval.h"
#include "obj-util.h"
#include "object.h"
#include "option.h"
#include "parser.h"
#include "player-spell.h"
#include "project.h"

static const char *mon_race_flags[] =
{
	#define RF(a, b, c) #a,
	#include "list-mon-race-flags.h"
	#undef RF
	NULL
};

static const char *obj_flags[] = {
	"NONE",
	#define STAT(a, b, c, d, e, f, g, h, i) #c,
	#include "list-stats.h"
	#undef STAT
	#define OF(a, b, c, d, e, f) #a,
	#include "list-object-flags.h"
	#undef OF
	NULL
};

static const char *obj_mods[] = {
	#define STAT(a, b, c, d, e, f, g, h, i) #a,
	#include "list-stats.h"
	#undef STAT
	#define OBJ_MOD(a, b, c, d) #a,
	#include "list-object-modifiers.h"
	#undef OBJ_MOD
	NULL
};

static const char *kind_flags[] = {
	#define KF(a, b) #a,
	#include "list-kind-flags.h"
	#undef KF
	NULL
};

static const char *elements[] = {
	#define ELEM(a, b, c, d, e, f, g, h, i, col) #a,
	#include "list-elements.h"
	#undef ELEM
	NULL
};

static bool grab_element_flag(struct element_info *info, const char *flag_name)
{
	char prefix[20];
	char suffix[20];
	size_t i;

	if (2 != sscanf(flag_name, "%[^_]_%s", prefix, suffix))
		return false;

	/* Ignore or hate */
	for (i = 0; i < ELEM_MAX; i++)
		if (streq(suffix, elements[i])) {
			if (streq(prefix, "IGNORE")) {
				info[i].flags |= EL_INFO_IGNORE;
				return true;
			}
			if (streq(prefix, "HATES")) {
				info[i].flags |= EL_INFO_HATES;
				return true;
			}
		}

	return false;
}

static enum parser_error write_dummy_object_record(struct artifact *art, const char *name)
{
	struct object_kind *temp, *dummy;
	int i;
	char mod_name[100];

	/* Extend by 1 and realloc */
	z_info->k_max += 1;
	temp = mem_realloc(k_info, (z_info->k_max + 1) * sizeof(*temp));

	/* Copy if no errors */
	if (!temp)
		return PARSE_ERROR_INTERNAL;
	else
		k_info = temp;

	/* Use the (second) last entry for the dummy */
	dummy = &k_info[z_info->k_max - 1];
	memset(dummy, 0, sizeof(*dummy));

	/* Copy the tval and base */
	dummy->tval = art->tval;
	dummy->base = &kb_info[dummy->tval];

	/* Make the name and index */
	my_strcpy(mod_name, format("& %s~", name), sizeof(mod_name));
	dummy->name = string_make(mod_name);
	dummy->kidx = z_info->k_max - 1;

	/* Increase the sval count for this tval, set the new one to the max */
	for (i = 0; i < TV_MAX; i++)
		if (kb_info[i].tval == dummy->tval) {
			kb_info[i].num_svals++;
			dummy->sval = kb_info[i].num_svals;
			break;
		}
	if (i == TV_MAX) return PARSE_ERROR_INTERNAL;

	/* Copy the sval to the artifact info */
	art->sval = dummy->sval;

	/* Give the object default colours (these should be overwritten) */
	dummy->d_char = '*';
	dummy->d_attr = COLOUR_RED;

	/* Register this as an INSTA_ART object */
	kf_on(dummy->kind_flags, KF_INSTA_ART);

	return PARSE_ERROR_NONE;
}

/**
 * Fill in curse object info now that curse_object_kind is defined
 */
static void write_curse_kinds(void)
{
	int i;
	int sval =  lookup_sval(tval_find_idx("none"), "<curse object>");

	for (i = 1; i < z_info->curse_max; i++) {
		struct curse *curse = &curses[i];
		curse->obj->kind = curse_object_kind;
		curse->obj->sval = sval;
		curse->obj->known = object_new();
		curse->obj->known->kind = curse_object_kind;
		curses[i].obj->known->sval = sval;
	}
}

static struct activation *findact(const char *act_name) {
	struct activation *act = &activations[1];
	while (act) {
		if (streq(act->name, act_name))
			break;
		act = act->next;
	}
	return act;
}

/**
 * ------------------------------------------------------------------------
 * Initialize object bases
 * ------------------------------------------------------------------------ */

struct kb_parsedata {
	struct object_base defaults;
	struct object_base *kb;
};

static enum parser_error parse_object_base_defaults(struct parser *p) {
	const char *label;
	int value;

	struct kb_parsedata *d = parser_priv(p);
	assert(d);

	label = parser_getsym(p, "label");
	value = parser_getint(p, "value");

	if (streq(label, "break-chance"))
		d->defaults.break_perc = value;
	else
		return PARSE_ERROR_UNDEFINED_DIRECTIVE;

	return PARSE_ERROR_NONE;
}

static enum parser_error parse_object_base_name(struct parser *p) {
	struct object_base *kb;

	struct kb_parsedata *d = parser_priv(p);
	assert(d);

	kb = mem_alloc(sizeof *kb);
	memcpy(kb, &d->defaults, sizeof(*kb));
	kb->next = d->kb;
	d->kb = kb;

	kb->tval = tval_find_idx(parser_getsym(p, "tval"));
	if (kb->tval == -1)
		return PARSE_ERROR_UNRECOGNISED_TVAL;

	if (parser_hasval(p, "name"))
		kb->name = string_make(parser_getstr(p, "name"));
	kb->num_svals = 0;

	return PARSE_ERROR_NONE;
}

static enum parser_error parse_object_base_graphics(struct parser *p) {
	struct object_base *kb;
	const char *color;

	struct kb_parsedata *d = parser_priv(p);
	assert(d);

	kb = d->kb;
	assert(kb);

	color = parser_getsym(p, "color");
	if (strlen(color) > 1)
		kb->attr = color_text_to_attr(color);
	else
		kb->attr = color_char_to_attr(color[0]);

	return PARSE_ERROR_NONE;
}

static enum parser_error parse_object_base_break(struct parser *p) {
	struct object_base *kb;

	struct kb_parsedata *d = parser_priv(p);
	assert(d);

	kb = d->kb;
	assert(kb);

	kb->break_perc = parser_getint(p, "breakage");

	return PARSE_ERROR_NONE;
}

static enum parser_error parse_object_base_flags(struct parser *p) {
	struct object_base *kb;
	char *s, *t;

	struct kb_parsedata *d = parser_priv(p);
	assert(d);

	kb = d->kb;
	assert(kb);

	s = string_make(parser_getstr(p, "flags"));
	t = strtok(s, " |");
	while (t) {
		bool found = false;
		if (!grab_flag(kb->flags, OF_SIZE, obj_flags, t))
			found = true;
		if (!grab_flag(kb->kind_flags, KF_SIZE, kind_flags, t))
			found = true;
		if (grab_element_flag(kb->el_info, t))
			found = true;
		if (!found)
			break;
		t = strtok(NULL, " |");
	}
	mem_free(s);

	return t ? PARSE_ERROR_INVALID_FLAG : PARSE_ERROR_NONE;
}

struct parser *init_parse_object_base(void) {
	struct parser *p = parser_new();

	struct kb_parsedata *d = mem_zalloc(sizeof(*d));
	parser_setpriv(p, d);

	parser_reg(p, "default sym label int value", parse_object_base_defaults);
	parser_reg(p, "name sym tval ?str name", parse_object_base_name);
	parser_reg(p, "graphics sym color", parse_object_base_graphics);
	parser_reg(p, "break int breakage", parse_object_base_break);
	parser_reg(p, "flags str flags", parse_object_base_flags);
	return p;
}

static errr run_parse_object_base(struct parser *p) {
	return parse_file_quit_not_found(p, "object_base");
}

static errr finish_parse_object_base(struct parser *p) {
	struct object_base *kb;
	struct object_base *next = NULL;
	struct kb_parsedata *d = parser_priv(p);

	assert(d);

	kb_info = mem_zalloc(TV_MAX * sizeof(*kb_info));

	for (kb = d->kb; kb; kb = next) {
		if (kb->tval >= TV_MAX)
			continue;
		memcpy(&kb_info[kb->tval], kb, sizeof(*kb));
		next = kb->next;
		mem_free(kb);
	}

	mem_free(d);
	parser_destroy(p);
	return 0;
}

static void cleanup_object_base(void)
{
	int idx;
	for (idx = 0; idx < TV_MAX; idx++)
	{
		string_free(kb_info[idx].name);
	}
	mem_free(kb_info);
}

struct file_parser object_base_parser = {
	"object_base",
	init_parse_object_base,
	run_parse_object_base,
	finish_parse_object_base,
	cleanup_object_base
};



/**
 * ------------------------------------------------------------------------
 * Initialize object slays
 * ------------------------------------------------------------------------ */

static enum parser_error parse_slay_code(struct parser *p) {
	const char *code = parser_getstr(p, "code");
	struct slay *h = parser_priv(p);
	struct slay *slay = mem_zalloc(sizeof *slay);

	slay->next = h;
	parser_setpriv(p, slay);
	slay->code = string_make(code);
	return PARSE_ERROR_NONE;
}

static enum parser_error parse_slay_name(struct parser *p) {
	const char *name = parser_getstr(p, "name");
	struct slay *slay = parser_priv(p);
	if (!slay)
		return PARSE_ERROR_MISSING_RECORD_HEADER;

	slay->name = string_make(name);
	return PARSE_ERROR_NONE;
}

static enum parser_error parse_slay_race_flag(struct parser *p) {
	int flag;
	struct slay *slay = parser_priv(p);
	if (!slay)
		return PARSE_ERROR_MISSING_RECORD_HEADER;

	flag = lookup_flag(mon_race_flags, parser_getsym(p, "flag"));

	if (flag == FLAG_END) {
		return PARSE_ERROR_INVALID_FLAG;
	} else {
		slay->race_flag = flag;
	}
	/* Flag or base, not both */
	if (slay->race_flag && slay->base)
		return PARSE_ERROR_INVALID_SLAY;

	return PARSE_ERROR_NONE;
}

static enum parser_error parse_slay_base(struct parser *p) {
	const char *base_name = parser_getsym(p, "base");
	struct slay *slay = parser_priv(p);

	slay->base = string_make(base_name);
	if (lookup_monster_base(base_name) == NULL)
		return PARSE_ERROR_INVALID_MONSTER_BASE;
	/* Flag or base, not both */
	if (slay->race_flag && slay->base)
		return PARSE_ERROR_INVALID_SLAY;

	return PARSE_ERROR_NONE;
}

static enum parser_error parse_slay_multiplier(struct parser *p) {
	struct slay *slay = parser_priv(p);
	if (!slay)
		return PARSE_ERROR_MISSING_RECORD_HEADER;

	slay->multiplier = parser_getuint(p, "multiplier");
	return PARSE_ERROR_NONE;
}

static enum parser_error parse_slay_power(struct parser *p) {
	struct slay *slay = parser_priv(p);
	if (!slay)
		return PARSE_ERROR_MISSING_RECORD_HEADER;

	slay->power = parser_getuint(p, "power");
	return PARSE_ERROR_NONE;
}

static enum parser_error parse_slay_melee_verb(struct parser *p) {
	const char *verb = parser_getstr(p, "verb");
	struct slay *slay = parser_priv(p);
	if (!slay)
		return PARSE_ERROR_MISSING_RECORD_HEADER;

	slay->melee_verb = string_make(verb);
	return PARSE_ERROR_NONE;
}

static enum parser_error parse_slay_range_verb(struct parser *p) {
	const char *verb = parser_getstr(p, "verb");
	struct slay *slay = parser_priv(p);
	if (!slay)
		return PARSE_ERROR_MISSING_RECORD_HEADER;

	slay->range_verb = string_make(verb);
	return PARSE_ERROR_NONE;
}

struct parser *init_parse_slay(void) {
	struct parser *p = parser_new();
	parser_setpriv(p, NULL);
	parser_reg(p, "code str code", parse_slay_code);
	parser_reg(p, "name str name", parse_slay_name);
	parser_reg(p, "race-flag sym flag", parse_slay_race_flag);
	parser_reg(p, "base sym base", parse_slay_base);
	parser_reg(p, "multiplier uint multiplier", parse_slay_multiplier);
	parser_reg(p, "power uint power", parse_slay_power);
	parser_reg(p, "melee-verb str verb", parse_slay_melee_verb);
	parser_reg(p, "range-verb str verb", parse_slay_range_verb);
	return p;
}

static errr run_parse_slay(struct parser *p) {
	return parse_file_quit_not_found(p, "slay");
}

static errr finish_parse_slay(struct parser *p) {
	struct slay *slay, *next = NULL;
	int count = 1;

	/* Count the entries */
	z_info->slay_max = 0;
	slay = parser_priv(p);
	while (slay) {
		z_info->slay_max++;
		slay = slay->next;
	}

	/* Allocate the direct access list and copy the data to it */
	slays = mem_zalloc((z_info->slay_max + 1) * sizeof(*slay));
	for (slay = parser_priv(p); slay; slay = next, count++) {
		memcpy(&slays[count], slay, sizeof(*slay));
		next = slay->next;
		slays[count].next = NULL;

		mem_free(slay);
	}
	z_info->slay_max += 1;

	parser_destroy(p);
	return 0;
}

static void cleanup_slay(void)
{
	int idx;
	for (idx = 0; idx < z_info->slay_max; idx++) {
		string_free(slays[idx].code);
		string_free(slays[idx].name);
		if (slays[idx].base)
			string_free(slays[idx].base);
		string_free(slays[idx].melee_verb);
		string_free(slays[idx].range_verb);
	}
	mem_free(slays);
}

struct file_parser slay_parser = {
	"slay",
	init_parse_slay,
	run_parse_slay,
	finish_parse_slay,
	cleanup_slay
};

/**
 * ------------------------------------------------------------------------
 * Initialize object brands
 * ------------------------------------------------------------------------ */

static enum parser_error parse_brand_code(struct parser *p) {
	const char *code = parser_getstr(p, "code");
	struct brand *h = parser_priv(p);
	struct brand *brand = mem_zalloc(sizeof *brand);

	brand->next = h;
	parser_setpriv(p, brand);
	brand->code = string_make(code);
	return PARSE_ERROR_NONE;
}

static enum parser_error parse_brand_name(struct parser *p) {
	const char *name = parser_getstr(p, "name");
	struct brand *brand = parser_priv(p);
	if (!brand)
		return PARSE_ERROR_MISSING_RECORD_HEADER;

	brand->name = string_make(name);
	return PARSE_ERROR_NONE;
}

static enum parser_error parse_brand_verb(struct parser *p) {
	const char *verb = parser_getstr(p, "verb");
	struct brand *brand = parser_priv(p);
	if (!brand)
		return PARSE_ERROR_MISSING_RECORD_HEADER;

	brand->verb = string_make(verb);
	return PARSE_ERROR_NONE;
}

static enum parser_error parse_brand_multiplier(struct parser *p) {
	struct brand *brand = parser_priv(p);
	if (!brand)
		return PARSE_ERROR_MISSING_RECORD_HEADER;

	brand->multiplier = parser_getuint(p, "multiplier");
	return PARSE_ERROR_NONE;
}

static enum parser_error parse_brand_power(struct parser *p) {
	struct brand *brand = parser_priv(p);
	if (!brand)
		return PARSE_ERROR_MISSING_RECORD_HEADER;

	brand->power = parser_getuint(p, "power");
	return PARSE_ERROR_NONE;
}

static enum parser_error parse_brand_resist_flag(struct parser *p) {
	int flag;
	struct brand *brand = parser_priv(p);
	if (!brand)
		return PARSE_ERROR_MISSING_RECORD_HEADER;

	flag = lookup_flag(mon_race_flags, parser_getsym(p, "flag"));

	if (flag == FLAG_END) {
		return PARSE_ERROR_INVALID_FLAG;
	} else {
		brand->resist_flag = flag;
	}

	return PARSE_ERROR_NONE;
}

struct parser *init_parse_brand(void) {
	struct parser *p = parser_new();
	parser_setpriv(p, NULL);
	parser_reg(p, "code str code", parse_brand_code);
	parser_reg(p, "name str name", parse_brand_name);
	parser_reg(p, "verb str verb", parse_brand_verb);
	parser_reg(p, "multiplier uint multiplier", parse_brand_multiplier);
	parser_reg(p, "power uint power", parse_brand_power);
	parser_reg(p, "resist-flag sym flag", parse_brand_resist_flag);
	return p;
}

static errr run_parse_brand(struct parser *p) {
	return parse_file_quit_not_found(p, "brand");
}

static errr finish_parse_brand(struct parser *p) {
	struct brand *brand, *next = NULL;
	int count = 1;

	/* Count the entries */
	z_info->brand_max = 0;
	brand = parser_priv(p);
	while (brand) {
		z_info->brand_max++;
		brand = brand->next;
	}

	/* Allocate the direct access list and copy the data to it */
	brands = mem_zalloc((z_info->brand_max + 1) * sizeof(*brand));
	for (brand = parser_priv(p); brand; brand = next, count++) {
		memcpy(&brands[count], brand, sizeof(*brand));
		next = brand->next;
		brands[count].next = NULL;

		mem_free(brand);
	}
	z_info->brand_max += 1;

	parser_destroy(p);
	return 0;
}

static void cleanup_brand(void)
{
	int idx;
	for (idx = 0; idx < z_info->brand_max; idx++) {
		string_free(brands[idx].code);
		string_free(brands[idx].name);
		string_free(brands[idx].verb);
	}
	mem_free(brands);
}

struct file_parser brand_parser = {
	"brand",
	init_parse_brand,
	run_parse_brand,
	finish_parse_brand,
	cleanup_brand
};



/**
 * ------------------------------------------------------------------------
 * Initialize object curses
 * ------------------------------------------------------------------------ */

static enum parser_error parse_curse_name(struct parser *p) {
	const char *name = parser_getstr(p, "name");
	struct curse *h = parser_priv(p);

	struct curse *curse = mem_zalloc(sizeof *curse);
	curse->obj = mem_zalloc(sizeof(struct object));
	curse->next = h;
	parser_setpriv(p, curse);
	curse->name = string_make(name);
	curse->poss = mem_zalloc(TV_MAX * sizeof(bool));

	return PARSE_ERROR_NONE;
}

static enum parser_error parse_curse_type(struct parser *p) {
	int tval = tval_find_idx(parser_getsym(p, "tval"));

	struct curse *curse = parser_priv(p);
	if (!curse)
		return PARSE_ERROR_MISSING_RECORD_HEADER;
	if ((tval < 0) || (tval >= TV_MAX))
		return PARSE_ERROR_UNRECOGNISED_TVAL;
	curse->poss[tval] = true;
	return PARSE_ERROR_NONE;
}

static enum parser_error parse_curse_combat(struct parser *p) {
	struct curse *curse = parser_priv(p);
	assert(curse);

	curse->obj->to_h = parser_getint(p, "to-h");
	curse->obj->to_d = parser_getint(p, "to-d");
	curse->obj->to_a = parser_getint(p, "to-a");
	return PARSE_ERROR_NONE;
}

static enum parser_error parse_curse_flags(struct parser *p) {
	struct curse *curse = parser_priv(p);
	char *s = string_make(parser_getstr(p, "flags"));
	char *t;
	assert(curse);

	t = strtok(s, " |");
	while (t) {
		bool found = false;
		if (!grab_flag(curse->obj->flags, OF_SIZE, obj_flags, t))
			found = true;
		if (grab_element_flag(curse->obj->el_info, t))
			found = true;
		if (!found)
			break;
		t = strtok(NULL, " |");
	}
	mem_free(s);
	return t ? PARSE_ERROR_INVALID_FLAG : PARSE_ERROR_NONE;
}

static enum parser_error parse_curse_values(struct parser *p) {
	struct curse *curse = parser_priv(p);
	char *s;
	char *t;
	assert(curse);

	s = string_make(parser_getstr(p, "values"));
	t = strtok(s, " |");

	while (t) {
		int value = 0;
		int index = 0;
		bool found = false;
		if (!grab_index_and_int(&value, &index, obj_mods, "", t)) {
			found = true;
			curse->obj->modifiers[index] = value;
		}
		if (!grab_index_and_int(&value, &index, elements, "RES_", t)) {
			found = true;
			curse->obj->el_info[index].res_level = value;
		}
		if (!found)
			break;

		t = strtok(NULL, " |");
	}

	mem_free(s);
	return t ? PARSE_ERROR_INVALID_VALUE : PARSE_ERROR_NONE;
}

static enum parser_error parse_curse_effect(struct parser *p) {
	struct curse *curse = parser_priv(p);
	struct effect *effect;
	struct effect *new_effect = mem_zalloc(sizeof(*new_effect));

	if (!curse)
		return PARSE_ERROR_MISSING_RECORD_HEADER;

	/* Go to the next vacant effect and set it to the new one  */
	if (curse->obj->effect) {
		effect = curse->obj->effect;
		while (effect->next)
			effect = effect->next;
		effect->next = new_effect;
	} else
		curse->obj->effect = new_effect;

	/* Fill in the detail */
	return grab_effect_data(p, new_effect);
}

static enum parser_error parse_curse_param(struct parser *p) {
	struct curse *curse = parser_priv(p);
	struct effect *effect = curse->obj->effect;

	if (!curse)
		return PARSE_ERROR_MISSING_RECORD_HEADER;

	/* If there is no effect, assume that this is human and not parser error. */
	if (effect == NULL)
		return PARSE_ERROR_NONE;

	while (effect->next) effect = effect->next;
	effect->params[1] = parser_getint(p, "p2");

	if (parser_hasval(p, "p3"))
		effect->params[2] = parser_getint(p, "p3");

	return PARSE_ERROR_NONE;
}


static enum parser_error parse_curse_dice(struct parser *p) {
	struct curse *curse = parser_priv(p);
	struct effect *effect = curse->obj->effect;
	dice_t *dice = NULL;
	const char *string = NULL;

	if (!curse)
		return PARSE_ERROR_MISSING_RECORD_HEADER;

	dice = dice_new();

	if (dice == NULL)
		return PARSE_ERROR_INVALID_DICE;

	/* Go to the correct effect */
	while (effect->next) effect = effect->next;

	string = parser_getstr(p, "dice");

	if (dice_parse_string(dice, string)) {
		effect->dice = dice;
	}
	else {
		dice_free(dice);
		return PARSE_ERROR_INVALID_DICE;
	}

	return PARSE_ERROR_NONE;
}

static enum parser_error parse_curse_expr(struct parser *p) {
	struct curse *curse = parser_priv(p);
	expression_t *expression = NULL;
	expression_base_value_f function = NULL;
	struct effect *effect = curse->obj->effect;
	const char *name;
	const char *base;
	const char *expr;

	if (!curse)
		return PARSE_ERROR_MISSING_RECORD_HEADER;

	/* If there are no dice, assume that this is human and not parser error. */
	if (curse->obj->effect->dice == NULL)
		return PARSE_ERROR_NONE;

	/* Go to the correct effect */
	while (effect->next) effect = effect->next;

	name = parser_getsym(p, "name");
	base = parser_getsym(p, "base");
	expr = parser_getstr(p, "expr");
	expression = expression_new();

	if (expression == NULL)
		return PARSE_ERROR_INVALID_EXPRESSION;

	function = spell_value_base_by_name(base);
	expression_set_base_value(expression, function);

	if (expression_add_operations_string(expression, expr) < 0)
		return PARSE_ERROR_BAD_EXPRESSION_STRING;

	if (dice_bind_expression(effect->dice, name, expression) < 0)
		return PARSE_ERROR_UNBOUND_EXPRESSION;

	/* The dice object makes a deep copy of the expression, so we can free it */
	expression_free(expression);

	return PARSE_ERROR_NONE;
}

static enum parser_error parse_curse_msg(struct parser *p) {
	struct curse *curse = parser_priv(p);
	assert(curse);
	curse->obj->effect_msg = string_append(curse->obj->effect_msg,
										   parser_getstr(p, "text"));
	return PARSE_ERROR_NONE;
}

static enum parser_error parse_curse_time(struct parser *p) {
	struct curse *curse = parser_priv(p);
	assert(curse);

	curse->obj->time = parser_getrand(p, "time");
	return PARSE_ERROR_NONE;
}

static enum parser_error parse_curse_desc(struct parser *p) {
	struct curse *curse = parser_priv(p);

	if (!curse)
		return PARSE_ERROR_MISSING_RECORD_HEADER;

	curse->desc = string_append(curse->desc, parser_getstr(p, "desc"));
	return PARSE_ERROR_NONE;
}

struct parser *init_parse_curse(void) {
	struct parser *p = parser_new();
	parser_setpriv(p, NULL);
	parser_reg(p, "name str name", parse_curse_name);
	parser_reg(p, "type sym tval", parse_curse_type);
	parser_reg(p, "combat int to-h int to-d int to-a", parse_curse_combat);
	parser_reg(p, "effect sym eff ?sym type ?int xtra", parse_curse_effect);
	parser_reg(p, "param int p2 ?int p3", parse_curse_param);
	parser_reg(p, "dice str dice", parse_curse_dice);
	parser_reg(p, "expr sym name sym base str expr", parse_curse_expr);
	parser_reg(p, "msg str text", parse_curse_msg);
	parser_reg(p, "time rand time", parse_curse_time);
	parser_reg(p, "flags str flags", parse_curse_flags);
	parser_reg(p, "values str values", parse_curse_values);
	parser_reg(p, "desc str desc", parse_curse_desc);
	return p;
}

static errr run_parse_curse(struct parser *p) {
	return parse_file_quit_not_found(p, "curse");
}

static errr finish_parse_curse(struct parser *p) {
	struct curse *curse, *next = NULL;
	int count = 1;

	/* Count the entries */
	z_info->curse_max = 0;
	curse = parser_priv(p);
	while (curse) {
		z_info->curse_max++;
		curse = curse->next;
	}

	/* Allocate the direct access list and copy the data to it */
	curses = mem_zalloc((z_info->curse_max + 1) * sizeof(*curse));
	for (curse = parser_priv(p); curse; curse = next, count++) {
		memcpy(&curses[count], curse, sizeof(*curse));
		next = curse->next;
		curses[count].next = NULL;

		mem_free(curse);
	}
	z_info->curse_max += 1;

	parser_destroy(p);
	return 0;
}

static void cleanup_curse(void)
{
	int idx;
	for (idx = 0; idx < z_info->curse_max; idx++) {
		string_free(curses[idx].name);
		mem_free(curses[idx].desc);
		if (curses[idx].obj) {
			if (curses[idx].obj->known) {
				free_effect(curses[idx].obj->known->effect);
				mem_free(curses[idx].obj->known->effect_msg);
				mem_free(curses[idx].obj->known);
			}
			free_effect(curses[idx].obj->effect);
			mem_free(curses[idx].obj->effect_msg);
			mem_free(curses[idx].obj);
		}
		mem_free(curses[idx].poss);
	}
	mem_free(curses);
}

struct file_parser curse_parser = {
	"curse",
	init_parse_curse,
	run_parse_curse,
	finish_parse_curse,
	cleanup_curse
};

/**
 * ------------------------------------------------------------------------
 * Initialize activations
 * ------------------------------------------------------------------------ */

static enum parser_error parse_act_name(struct parser *p) {
	const char *name = parser_getstr(p, "name");
	struct activation *h = parser_priv(p);

	struct activation *act = mem_zalloc(sizeof *act);
	act->next = h;
	parser_setpriv(p, act);
	act->name = string_make(name);

	return PARSE_ERROR_NONE;
}

static enum parser_error parse_act_aim(struct parser *p) {
	struct activation *act = parser_priv(p);
	int val;
	assert(act);

	val = parser_getuint(p, "aim");
	act->aim = val ? true : false;
	return PARSE_ERROR_NONE;
}

static enum parser_error parse_act_power(struct parser *p) {
	struct activation *act = parser_priv(p);
	assert(act);

	act->power = parser_getuint(p, "power");
	return PARSE_ERROR_NONE;
}

static enum parser_error parse_act_effect(struct parser *p) {
	struct activation *act = parser_priv(p);
	struct effect *effect;
	struct effect *new_effect = mem_zalloc(sizeof(*new_effect));

	if (!act)
		return PARSE_ERROR_MISSING_RECORD_HEADER;

	/* Go to the next vacant effect and set it to the new one  */
	if (act->effect) {
		effect = act->effect;
		while (effect->next)
			effect = effect->next;
		effect->next = new_effect;
	} else
		act->effect = new_effect;

	/* Fill in the detail */
	return grab_effect_data(p, new_effect);
}

static enum parser_error parse_act_param(struct parser *p) {
	struct activation *act = parser_priv(p);
	struct effect *effect = act->effect;

	if (!act)
		return PARSE_ERROR_MISSING_RECORD_HEADER;

	/* If there is no effect, assume that this is human and not parser error. */
	if (effect == NULL)
		return PARSE_ERROR_NONE;

	while (effect->next) effect = effect->next;
	effect->params[1] = parser_getint(p, "p2");

	if (parser_hasval(p, "p3"))
		effect->params[2] = parser_getint(p, "p3");

	return PARSE_ERROR_NONE;
}


static enum parser_error parse_act_dice(struct parser *p) {
	struct activation *act = parser_priv(p);
	struct effect *effect = act->effect;
	dice_t *dice = NULL;
	const char *string = NULL;

	if (!act)
		return PARSE_ERROR_MISSING_RECORD_HEADER;

	dice = dice_new();

	if (dice == NULL)
		return PARSE_ERROR_INVALID_DICE;

	/* Go to the correct effect */
	while (effect->next) effect = effect->next;

	string = parser_getstr(p, "dice");

	if (dice_parse_string(dice, string)) {
		effect->dice = dice;
	}
	else {
		dice_free(dice);
		return PARSE_ERROR_INVALID_DICE;
	}

	return PARSE_ERROR_NONE;
}

static enum parser_error parse_act_expr(struct parser *p) {
	struct activation *act = parser_priv(p);
	expression_t *expression = NULL;
	expression_base_value_f function = NULL;
	struct effect *effect = act->effect;
	const char *name;
	const char *base;
	const char *expr;

	if (!act)
		return PARSE_ERROR_MISSING_RECORD_HEADER;

	/* If there are no dice, assume that this is human and not parser error. */
	if (act->effect->dice == NULL)
		return PARSE_ERROR_NONE;

	/* Go to the correct effect */
	while (effect->next) effect = effect->next;

	name = parser_getsym(p, "name");
	base = parser_getsym(p, "base");
	expr = parser_getstr(p, "expr");
	expression = expression_new();

	if (expression == NULL)
		return PARSE_ERROR_INVALID_EXPRESSION;

	function = spell_value_base_by_name(base);
	expression_set_base_value(expression, function);

	if (expression_add_operations_string(expression, expr) < 0)
		return PARSE_ERROR_BAD_EXPRESSION_STRING;

	if (dice_bind_expression(effect->dice, name, expression) < 0)
		return PARSE_ERROR_UNBOUND_EXPRESSION;

	/* The dice object makes a deep copy of the expression, so we can free it */
	expression_free(expression);

	return PARSE_ERROR_NONE;
}

static enum parser_error parse_act_msg(struct parser *p) {
	struct activation *act = parser_priv(p);
	assert(act);
	act->message = string_append(act->message, parser_getstr(p, "msg"));
	return PARSE_ERROR_NONE;
}


static enum parser_error parse_act_desc(struct parser *p) {
	struct activation *act = parser_priv(p);

	if (!act)
		return PARSE_ERROR_MISSING_RECORD_HEADER;

	act->desc = string_append(act->desc, parser_getstr(p, "desc"));
	return PARSE_ERROR_NONE;
}

struct parser *init_parse_act(void) {
	struct parser *p = parser_new();
	parser_setpriv(p, NULL);
	parser_reg(p, "name str name", parse_act_name);
	parser_reg(p, "aim uint aim", parse_act_aim);
	parser_reg(p, "power uint power", parse_act_power);
	parser_reg(p, "effect sym eff ?sym type ?int xtra", parse_act_effect);
	parser_reg(p, "param int p2 ?int p3", parse_act_param);
	parser_reg(p, "dice str dice", parse_act_dice);
	parser_reg(p, "expr sym name sym base str expr", parse_act_expr);
	parser_reg(p, "msg str msg", parse_act_msg);
	parser_reg(p, "desc str desc", parse_act_desc);
	return p;
}

static errr run_parse_act(struct parser *p) {
	return parse_file_quit_not_found(p, "activation");
}

static errr finish_parse_act(struct parser *p) {
	struct activation *act, *next = NULL;
	int count = 1;

	/* Count the entries */
	z_info->act_max = 0;
	act = parser_priv(p);
	while (act) {
		z_info->act_max++;
		act = act->next;
	}

	/* Allocate the direct access list and copy the data to it */
	activations = mem_zalloc((z_info->act_max + 1) * sizeof(*act));
	for (act = parser_priv(p); act; act = next, count++) {
		memcpy(&activations[count], act, sizeof(*act));
		activations[count].index = count;
		next = act->next;
		if (next)
			activations[count].next = &activations[count + 1];
		else
			activations[count].next = NULL;

		mem_free(act);
	}
	z_info->act_max += 1;

	parser_destroy(p);
	return 0;
}

static void cleanup_act(void)
{
	int idx;
	for (idx = 0; idx < z_info->act_max; idx++) {
		string_free(activations[idx].name);
		mem_free(activations[idx].desc);
		mem_free(activations[idx].message);
		free_effect(activations[idx].effect);
	}
	mem_free(activations);
}

struct file_parser act_parser = {
	"activation",
	init_parse_act,
	run_parse_act,
	finish_parse_act,
	cleanup_act
};

/**
 * ------------------------------------------------------------------------
 * Initialize objects
 * ------------------------------------------------------------------------ */

/* Generic object kinds */
struct object_kind *unknown_item_kind;
struct object_kind *unknown_gold_kind;
struct object_kind *pile_kind;
struct object_kind *curse_object_kind;

static enum parser_error parse_object_name(struct parser *p) {
	int idx = parser_getint(p, "index");
	const char *name = parser_getstr(p, "name");
	struct object_kind *h = parser_priv(p);

	struct object_kind *k = mem_zalloc(sizeof *k);
	k->next = h;
	parser_setpriv(p, k);
	k->kidx = idx;
	k->name = string_make(name);
	return PARSE_ERROR_NONE;
}

static enum parser_error parse_object_graphics(struct parser *p) {
	wchar_t glyph = parser_getchar(p, "glyph");
	const char *color = parser_getsym(p, "color");
	struct object_kind *k = parser_priv(p);
	assert(k);

	k->d_char = glyph;
	if (strlen(color) > 1)
		k->d_attr = color_text_to_attr(color);
	else
		k->d_attr = color_char_to_attr(color[0]);

	return PARSE_ERROR_NONE;
}

static enum parser_error parse_object_type(struct parser *p) {
	struct object_kind *k = parser_priv(p);
	int tval;

	assert(k);

	tval = tval_find_idx(parser_getsym(p, "tval"));
	if (tval < 0)
		return PARSE_ERROR_UNRECOGNISED_TVAL;

	k->tval = tval;
	k->base = &kb_info[k->tval];
	k->base->num_svals++;
	k->sval = k->base->num_svals;

	return PARSE_ERROR_NONE;
}

static enum parser_error parse_object_properties(struct parser *p) {
	struct object_kind *k = parser_priv(p);
	assert(k);

	k->level = parser_getint(p, "level");
	k->weight = parser_getint(p, "weight");
	k->cost = parser_getint(p, "cost");
	return PARSE_ERROR_NONE;
}

static enum parser_error parse_object_alloc(struct parser *p) {
	struct object_kind *k = parser_priv(p);
	const char *tmp = parser_getstr(p, "minmax");
	int amin, amax;
	assert(k);

	k->alloc_prob = parser_getint(p, "common");
	if (sscanf(tmp, "%d to %d", &amin, &amax) != 2)
		return PARSE_ERROR_INVALID_ALLOCATION;

	k->alloc_min = amin;
	k->alloc_max = amax;
	return PARSE_ERROR_NONE;
}

static enum parser_error parse_object_combat(struct parser *p) {
	struct object_kind *k = parser_priv(p);
	struct random hd = parser_getrand(p, "hd");
	assert(k);

	k->ac = parser_getint(p, "ac");
	k->dd = hd.dice;
	k->ds = hd.sides;
	k->to_h = parser_getrand(p, "to-h");
	k->to_d = parser_getrand(p, "to-d");
	k->to_a = parser_getrand(p, "to-a");
	return PARSE_ERROR_NONE;
}

static enum parser_error parse_object_charges(struct parser *p) {
	struct object_kind *k = parser_priv(p);
	assert(k);

	k->charge = parser_getrand(p, "charges");
	return PARSE_ERROR_NONE;
}

static enum parser_error parse_object_pile(struct parser *p) {
	struct object_kind *k = parser_priv(p);
	assert(k);

	k->gen_mult_prob = parser_getint(p, "prob");
	k->stack_size = parser_getrand(p, "stack");
	return PARSE_ERROR_NONE;
}

static enum parser_error parse_object_flags(struct parser *p) {
	struct object_kind *k = parser_priv(p);
	char *s = string_make(parser_getstr(p, "flags"));
	char *t;
	assert(k);

	t = strtok(s, " |");
	while (t) {
		bool found = false;
		if (!grab_flag(k->flags, OF_SIZE, obj_flags, t))
			found = true;
		if (!grab_flag(k->kind_flags, KF_SIZE, kind_flags, t))
			found = true;
		if (grab_element_flag(k->el_info, t))
			found = true;
		if (!found)
			break;
		t = strtok(NULL, " |");
	}
	mem_free(s);
	return t ? PARSE_ERROR_INVALID_FLAG : PARSE_ERROR_NONE;
}

static enum parser_error parse_object_power(struct parser *p) {
	struct object_kind *k = parser_priv(p);
	assert(k);

	k->power = parser_getint(p, "power");
	return PARSE_ERROR_NONE;
}

static enum parser_error parse_object_effect(struct parser *p) {
	struct object_kind *k = parser_priv(p);
	struct effect *effect;
	struct effect *new_effect = mem_zalloc(sizeof(*new_effect));

	if (!k)
		return PARSE_ERROR_MISSING_RECORD_HEADER;

	/* Go to the next vacant effect and set it to the new one  */
	if (k->effect) {
		effect = k->effect;
		while (effect->next)
			effect = effect->next;
		effect->next = new_effect;
	} else
		k->effect = new_effect;

	/* Fill in the detail */
	return grab_effect_data(p, new_effect);
}

static enum parser_error parse_object_param(struct parser *p) {
	struct object_kind *k = parser_priv(p);
	struct effect *effect = k->effect;

	if (!k)
		return PARSE_ERROR_MISSING_RECORD_HEADER;

	/* If there is no effect, assume that this is human and not parser error. */
	if (effect == NULL)
		return PARSE_ERROR_NONE;

	while (effect->next) effect = effect->next;
	effect->params[1] = parser_getint(p, "p2");

	if (parser_hasval(p, "p3"))
		effect->params[2] = parser_getint(p, "p3");

	return PARSE_ERROR_NONE;
}


static enum parser_error parse_object_dice(struct parser *p) {
	struct object_kind *k = parser_priv(p);
	dice_t *dice = NULL;
	struct effect *effect = k->effect;
	const char *string = NULL;

	if (!k)
		return PARSE_ERROR_MISSING_RECORD_HEADER;

	/* If there is no effect, assume that this is human and not parser error. */
	if (effect == NULL)
		return PARSE_ERROR_NONE;

	while (effect->next) effect = effect->next;

	dice = dice_new();

	if (dice == NULL)
		return PARSE_ERROR_INVALID_DICE;

	string = parser_getstr(p, "dice");

	if (dice_parse_string(dice, string)) {
		effect->dice = dice;
	}
	else {
		dice_free(dice);
		return PARSE_ERROR_INVALID_DICE;
	}

	return PARSE_ERROR_NONE;
}

static enum parser_error parse_object_expr(struct parser *p) {
	struct object_kind *k = parser_priv(p);
	struct effect *effect = k->effect;
	expression_t *expression = NULL;
	expression_base_value_f function = NULL;
	const char *name;
	const char *base;
	const char *expr;

	if (!k)
		return PARSE_ERROR_MISSING_RECORD_HEADER;

	/* If there is no effect, assume that this is human and not parser error. */
	if (effect == NULL)
		return PARSE_ERROR_NONE;

	while (effect->next) effect = effect->next;

	/* If there are no dice, assume that this is human and not parser error. */
	if (effect->dice == NULL)
		return PARSE_ERROR_NONE;

	name = parser_getsym(p, "name");
	base = parser_getsym(p, "base");
	expr = parser_getstr(p, "expr");
	expression = expression_new();

	if (expression == NULL)
		return PARSE_ERROR_INVALID_EXPRESSION;

	function = spell_value_base_by_name(base);
	expression_set_base_value(expression, function);

	if (expression_add_operations_string(expression, expr) < 0)
		return PARSE_ERROR_BAD_EXPRESSION_STRING;

	if (dice_bind_expression(effect->dice, name, expression) < 0)
		return PARSE_ERROR_UNBOUND_EXPRESSION;

	/* The dice object makes a deep copy of the expression, so we can free it */
	expression_free(expression);

	return PARSE_ERROR_NONE;
}

static enum parser_error parse_object_msg(struct parser *p) {
	struct object_kind *k = parser_priv(p);
	assert(k);
	k->effect_msg = string_append(k->effect_msg, parser_getstr(p, "text"));
	return PARSE_ERROR_NONE;
}

static enum parser_error parse_object_time(struct parser *p) {
	struct object_kind *k = parser_priv(p);
	assert(k);

	k->time = parser_getrand(p, "time");
	return PARSE_ERROR_NONE;
}

static enum parser_error parse_object_desc(struct parser *p) {
	struct object_kind *k = parser_priv(p);
	assert(k);
	k->text = string_append(k->text, parser_getstr(p, "text"));
	return PARSE_ERROR_NONE;
}

static enum parser_error parse_object_pval(struct parser *p) {
	struct object_kind *k = parser_priv(p);
	assert(k);

	k->pval = parser_getrand(p, "pval");
	return PARSE_ERROR_NONE;
}

static enum parser_error parse_object_values(struct parser *p) {
	struct object_kind *k = parser_priv(p);
	char *s;
	char *t;
	assert(k);

	s = string_make(parser_getstr(p, "values"));
	t = strtok(s, " |");

	while (t) {
		int value = 0;
		int index = 0;
		bool found = false;
		if (!grab_rand_value(k->modifiers, obj_mods, t))
			found = true;
		if (!grab_index_and_int(&value, &index, elements, "RES_", t)) {
			found = true;
			k->el_info[index].res_level = value;
		}
		if (!found)
			break;

		t = strtok(NULL, " |");
	}

	mem_free(s);
	return t ? PARSE_ERROR_INVALID_VALUE : PARSE_ERROR_NONE;
}

static enum parser_error parse_object_slay(struct parser *p) {
	struct object_kind *k = parser_priv(p);
	const char *s = parser_getstr(p, "code");
	int i;

	assert(k);
	for (i = 1; i < z_info->slay_max; i++) {
		if (streq(s, slays[i].code)) break;
	}
	if (i == z_info->slay_max)
		return PARSE_ERROR_UNRECOGNISED_SLAY;

	if (!k->slays)
		k->slays = mem_zalloc(z_info->slay_max * sizeof(bool));
	k->slays[i] = true;
	return PARSE_ERROR_NONE;
}

static enum parser_error parse_object_brand(struct parser *p) {
	struct object_kind *k = parser_priv(p);
	const char *s = parser_getstr(p, "code");
	int i;

	assert(k);
	for (i = 1; i < z_info->brand_max; i++) {
		if (streq(s, brands[i].code)) break;
	}
	if (i == z_info->brand_max)
		return PARSE_ERROR_UNRECOGNISED_BRAND;

	if (!k->brands)
		k->brands = mem_zalloc(z_info->brand_max * sizeof(bool));
	k->brands[i] = true;
	return PARSE_ERROR_NONE;
}

static enum parser_error parse_object_curse(struct parser *p) {
	struct object_kind *k = parser_priv(p);
	const char *s = parser_getsym(p, "name");
	int i;

	assert(k);
	for (i = 1; i < z_info->curse_max; i++) {
		if (streq(s, curses[i].name)) break;
	}
	if (i == z_info->curse_max)
		return PARSE_ERROR_UNRECOGNISED_CURSE;

	if (!k->curses)
		k->curses = mem_zalloc(z_info->curse_max * sizeof(int));
	k->curses[i] = parser_getint(p, "power");
	return PARSE_ERROR_NONE;
}


struct parser *init_parse_object(void) {
	struct parser *p = parser_new();
	parser_setpriv(p, NULL);
	parser_reg(p, "name int index str name", parse_object_name);
	parser_reg(p, "graphics char glyph sym color", parse_object_graphics);
	parser_reg(p, "type sym tval", parse_object_type);
	parser_reg(p, "properties int level int weight int cost", parse_object_properties);
	parser_reg(p, "alloc int common str minmax", parse_object_alloc);
	parser_reg(p, "combat int ac rand hd rand to-h rand to-d rand to-a", parse_object_combat);
	parser_reg(p, "charges rand charges", parse_object_charges);
	parser_reg(p, "pile int prob rand stack", parse_object_pile);
	parser_reg(p, "flags str flags", parse_object_flags);
	parser_reg(p, "power int power", parse_object_power);
	parser_reg(p, "effect sym eff ?sym type ?int xtra", parse_object_effect);
	parser_reg(p, "param int p2 ?int p3", parse_object_param);
	parser_reg(p, "dice str dice", parse_object_dice);
	parser_reg(p, "expr sym name sym base str expr", parse_object_expr);
	parser_reg(p, "msg str text", parse_object_msg);
	parser_reg(p, "time rand time", parse_object_time);
	parser_reg(p, "pval rand pval", parse_object_pval);
	parser_reg(p, "values str values", parse_object_values);
	parser_reg(p, "desc str text", parse_object_desc);
	parser_reg(p, "slay str code", parse_object_slay);
	parser_reg(p, "brand str code", parse_object_brand);
	parser_reg(p, "curse sym name int power", parse_object_curse);
	return p;
}

static errr run_parse_object(struct parser *p) {
	return parse_file_quit_not_found(p, "object");
}

static errr finish_parse_object(struct parser *p) {
	struct object_kind *k, *next = NULL;

	/* scan the list for the max id */
	z_info->k_max = 0;
	k = parser_priv(p);
	while (k) {
		if (k->kidx > z_info->k_max)
			z_info->k_max = k->kidx;
		k = k->next;
	}

	/* allocate the direct access list and copy the data to it */
	k_info = mem_zalloc((z_info->k_max + 1) * sizeof(*k));
	for (k = parser_priv(p); k; k = next) {
		memcpy(&k_info[k->kidx], k, sizeof(*k));

		/* Add base kind flags to kind kind flags */
		kf_union(k_info[k->kidx].kind_flags, kb_info[k->tval].kind_flags);

		next = k->next;
		if (next)
			k_info[k->kidx].next = &k_info[next->kidx];
		else
			k_info[k->kidx].next = NULL;
		mem_free(k);
	}
	z_info->k_max += 1;

	/*objkinds = parser_priv(p); not used yet, when used, remove the mem_free(k); above */
	parser_destroy(p);
	return 0;
}

static void cleanup_object(void)
{
	int idx;
	for (idx = 0; idx < z_info->k_max; idx++) {
		struct object_kind *kind = &k_info[idx];
		string_free(kind->name);
		string_free(kind->text);
		string_free(kind->effect_msg);
		mem_free(kind->brands);
		mem_free(kind->slays);
		mem_free(kind->curses);
		free_effect(kind->effect);
	}
	mem_free(k_info);
}

struct file_parser object_parser = {
	"object",
	init_parse_object,
	run_parse_object,
	finish_parse_object,
	cleanup_object
};

/**
 * ------------------------------------------------------------------------
 * Initialize ego items
 * ------------------------------------------------------------------------ */

static enum parser_error parse_ego_name(struct parser *p) {
	int idx = parser_getint(p, "index");
	const char *name = parser_getstr(p, "name");
	struct ego_item *h = parser_priv(p);

	struct ego_item *e = mem_zalloc(sizeof *e);
	e->next = h;
	parser_setpriv(p, e);
	e->eidx = idx;
	e->name = string_make(name);
	return PARSE_ERROR_NONE;
}

static enum parser_error parse_ego_info(struct parser *p) {
	struct ego_item *e = parser_priv(p);
	if (!e) {
		return PARSE_ERROR_MISSING_RECORD_HEADER;
	}

	e->cost = parser_getint(p, "cost");
	e->rating = parser_getint(p, "rating");

	return PARSE_ERROR_NONE;
}

static enum parser_error parse_ego_alloc(struct parser *p) {
	struct ego_item *e = parser_priv(p);
	const char *tmp = parser_getstr(p, "minmax");
	int amin, amax;

	e->alloc_prob = parser_getint(p, "common");
	if (sscanf(tmp, "%d to %d", &amin, &amax) != 2)
		return PARSE_ERROR_INVALID_ALLOCATION;

	if (amin > 255 || amax > 255 || amin < 0 || amax < 0)
		return PARSE_ERROR_OUT_OF_BOUNDS;

	e->alloc_min = amin;
	e->alloc_max = amax;
	return PARSE_ERROR_NONE;
}

static enum parser_error parse_ego_type(struct parser *p) {
	struct poss_item *poss;
	int i;
	int tval = tval_find_idx(parser_getsym(p, "tval"));
	bool found_one_kind = false;

	struct ego_item *e = parser_priv(p);
	if (!e)
		return PARSE_ERROR_MISSING_RECORD_HEADER;
	if (tval < 0)
		return PARSE_ERROR_UNRECOGNISED_TVAL;

	/* Find all the right object kinds */
	for (i = 0; i < z_info->k_max; i++) {
		if (k_info[i].tval != tval) continue;
		poss = mem_zalloc(sizeof(struct poss_item));
		poss->kidx = i;
		poss->next = e->poss_items;
		e->poss_items = poss;
		found_one_kind = true;
	}

	if (!found_one_kind)
		return PARSE_ERROR_NO_KIND_FOR_EGO_TYPE;
	return PARSE_ERROR_NONE;
}

static enum parser_error parse_ego_item(struct parser *p) {
	struct poss_item *poss;
	int tval = tval_find_idx(parser_getsym(p, "tval"));
	int sval = lookup_sval(tval, parser_getsym(p, "sval"));

	struct ego_item *e = parser_priv(p);
	if (!e)
		return PARSE_ERROR_MISSING_RECORD_HEADER;
	if (tval < 0)
		return PARSE_ERROR_UNRECOGNISED_TVAL;

	poss = mem_zalloc(sizeof(struct poss_item));
	poss->kidx = lookup_kind(tval, sval)->kidx;
	poss->next = e->poss_items;
	e->poss_items = poss;

	if (poss->kidx <= 0)
		return PARSE_ERROR_INVALID_ITEM_NUMBER;
	return PARSE_ERROR_NONE;
}

static enum parser_error parse_ego_combat(struct parser *p) {
	struct random th = parser_getrand(p, "th");
	struct random td = parser_getrand(p, "td");
	struct random ta = parser_getrand(p, "ta");
	struct ego_item *e = parser_priv(p);

	if (!e)
		return PARSE_ERROR_MISSING_RECORD_HEADER;

	e->to_h = th;
	e->to_d = td;
	e->to_a = ta;

	return PARSE_ERROR_NONE;
}

static enum parser_error parse_ego_min(struct parser *p) {
	int th = parser_getint(p, "th");
	int td = parser_getint(p, "td");
	int ta = parser_getint(p, "ta");
	struct ego_item *e = parser_priv(p);

	if (!e)
		return PARSE_ERROR_MISSING_RECORD_HEADER;

	e->min_to_h = th;
	e->min_to_d = td;
	e->min_to_a = ta;

	return PARSE_ERROR_NONE;
}

static enum parser_error parse_ego_effect(struct parser *p) {
	struct ego_item *e = parser_priv(p);
	struct effect *effect;
	struct effect *new_effect = mem_zalloc(sizeof(*new_effect));

	if (!e)
		return PARSE_ERROR_MISSING_RECORD_HEADER;

	/* Go to the next vacant effect and set it to the new one  */
	if (e->effect) {
		effect = e->effect;
		while (effect->next)
			effect = effect->next;
		effect->next = new_effect;
	} else
		e->effect = new_effect;

	/* Fill in the detail */
	return grab_effect_data(p, new_effect);
}

static enum parser_error parse_ego_dice(struct parser *p) {
	struct ego_item *e = parser_priv(p);
	dice_t *dice = NULL;
	const char *string = NULL;

	if (!e)
		return PARSE_ERROR_MISSING_RECORD_HEADER;

	dice = dice_new();

	if (dice == NULL)
		return PARSE_ERROR_INVALID_DICE;

	string = parser_getstr(p, "dice");

	if (dice_parse_string(dice, string)) {
		e->effect->dice = dice;
	}
	else {
		dice_free(dice);
		return PARSE_ERROR_INVALID_DICE;
	}

	return PARSE_ERROR_NONE;
}

static enum parser_error parse_ego_time(struct parser *p) {
	struct ego_item *e = parser_priv(p);
	assert(e);

	e->time = parser_getrand(p, "time");
	return PARSE_ERROR_NONE;
}

static enum parser_error parse_ego_flags(struct parser *p) {
	struct ego_item *e = parser_priv(p);
	char *flags;
	char *t;

	if (!e)
		return PARSE_ERROR_MISSING_RECORD_HEADER;
	if (!parser_hasval(p, "flags"))
		return PARSE_ERROR_NONE;
	flags = string_make(parser_getstr(p, "flags"));
	t = strtok(flags, " |");
	while (t) {
		bool found = false;
		if (!grab_flag(e->flags, OF_SIZE, obj_flags, t))
			found = true;
		if (!grab_flag(e->kind_flags, KF_SIZE, kind_flags, t))
			found = true;
		if (grab_element_flag(e->el_info, t))
			found = true;
		if (!found)
			break;
		t = strtok(NULL, " |");
	}
	mem_free(flags);
	return t ? PARSE_ERROR_INVALID_FLAG : PARSE_ERROR_NONE;
}

static enum parser_error parse_ego_flags_off(struct parser *p) {
	struct ego_item *e = parser_priv(p);
	char *flags;
	char *t;

	if (!e)
		return PARSE_ERROR_MISSING_RECORD_HEADER;
	if (!parser_hasval(p, "flags"))
		return PARSE_ERROR_NONE;
	flags = string_make(parser_getstr(p, "flags"));
	t = strtok(flags, " |");
	while (t) {
		if (grab_flag(e->flags_off, OF_SIZE, obj_flags, t))
			return PARSE_ERROR_INVALID_FLAG;
		t = strtok(NULL, " |");
	}
	mem_free(flags);
	return PARSE_ERROR_NONE;
}

static enum parser_error parse_ego_values(struct parser *p) {
	struct ego_item *e = parser_priv(p);
	char *s; 
	char *t;

	if (!e)
		return PARSE_ERROR_MISSING_RECORD_HEADER;
	if (!parser_hasval(p, "values"))
		return PARSE_ERROR_MISSING_FIELD;

	s = string_make(parser_getstr(p, "values"));
	t = strtok(s, " |");

	while (t) {
		bool found = false;
		int value = 0;
		int index = 0;
		if (!grab_rand_value(e->modifiers, obj_mods, t))
			found = true;
		if (!grab_index_and_int(&value, &index, elements, "RES_", t)) {
			found = true;
			e->el_info[index].res_level = value;
		}
		if (!found)
			break;

		t = strtok(NULL, " |");
	}

	mem_free(s);
	return t ? PARSE_ERROR_INVALID_VALUE : PARSE_ERROR_NONE;
}

static enum parser_error parse_ego_min_val(struct parser *p) {
	struct ego_item *e = parser_priv(p);
	char *s; 
	char *t;

	if (!e)
		return PARSE_ERROR_MISSING_RECORD_HEADER;
	if (!parser_hasval(p, "min_values"))
		return PARSE_ERROR_MISSING_FIELD;

	s = string_make(parser_getstr(p, "min_values"));
	t = strtok(s, " |");

	while (t) {
		bool found = false;
		if (!grab_int_value(e->min_modifiers, obj_mods, t))
			found = true;
		if (!found)
			break;

		t = strtok(NULL, " |");
	}

	mem_free(s);
	return t ? PARSE_ERROR_INVALID_VALUE : PARSE_ERROR_NONE;
}

static enum parser_error parse_ego_desc(struct parser *p) {
	struct ego_item *e = parser_priv(p);

	if (!e)
		return PARSE_ERROR_MISSING_RECORD_HEADER;
	e->text = string_append(e->text, parser_getstr(p, "text"));
	return PARSE_ERROR_NONE;
}

static enum parser_error parse_ego_slay(struct parser *p) {
	struct ego_item *e = parser_priv(p);
	const char *s = parser_getstr(p, "code");
	int i;

	if (!e)
		return PARSE_ERROR_MISSING_RECORD_HEADER;
	for (i = 1; i < z_info->slay_max; i++) {
		if (streq(s, slays[i].code)) break;
	}
	if (i == z_info->slay_max)
		return PARSE_ERROR_UNRECOGNISED_SLAY;

	if (!e->slays)
		e->slays = mem_zalloc(z_info->slay_max * sizeof(bool));
	e->slays[i] = true;
	return PARSE_ERROR_NONE;
}

static enum parser_error parse_ego_brand(struct parser *p) {
	struct ego_item *e = parser_priv(p);
	const char *s = parser_getstr(p, "code");
	int i;

	if (!e)
		return PARSE_ERROR_MISSING_RECORD_HEADER;
	for (i = 1; i < z_info->brand_max; i++) {
		if (streq(s, brands[i].code)) break;
	}
	if (i == z_info->brand_max)
		return PARSE_ERROR_UNRECOGNISED_BRAND;

	if (!e->brands)
		e->brands = mem_zalloc(z_info->brand_max * sizeof(bool));
	e->brands[i] = true;
	return PARSE_ERROR_NONE;
}

static enum parser_error parse_ego_curse(struct parser *p) {
	struct ego_item *e = parser_priv(p);
	char *s;
	int i;

	if (!e)
		return PARSE_ERROR_MISSING_RECORD_HEADER;
	s = string_make(parser_getsym(p, "name"));
	for (i = 1; i < z_info->curse_max; i++) {
		if (streq(s, curses[i].name)) break;
	}
	if (i == z_info->curse_max)
		return PARSE_ERROR_UNRECOGNISED_CURSE;

	if (!e->curses)
		e->curses = mem_zalloc(z_info->curse_max * sizeof(int));
	e->curses[i] = parser_getint(p, "power");
	return PARSE_ERROR_NONE;
}

struct parser *init_parse_ego(void) {
	struct parser *p = parser_new();
	parser_setpriv(p, NULL);
	parser_reg(p, "name int index str name", parse_ego_name);
	parser_reg(p, "info int cost int rating", parse_ego_info);
	parser_reg(p, "alloc int common str minmax", parse_ego_alloc);
	parser_reg(p, "type sym tval", parse_ego_type);
	parser_reg(p, "item sym tval sym sval", parse_ego_item);
	parser_reg(p, "combat rand th rand td rand ta", parse_ego_combat);
	parser_reg(p, "min-combat int th int td int ta", parse_ego_min);
	parser_reg(p, "effect sym eff ?sym type ?int xtra", parse_ego_effect);
	parser_reg(p, "dice str dice", parse_ego_dice);
	parser_reg(p, "time rand time", parse_ego_time);
	parser_reg(p, "flags ?str flags", parse_ego_flags);
	parser_reg(p, "flags-off ?str flags", parse_ego_flags_off);
	parser_reg(p, "values str values", parse_ego_values);
	parser_reg(p, "min-values str min_values", parse_ego_min_val);
	parser_reg(p, "desc str text", parse_ego_desc);
	parser_reg(p, "slay str code", parse_ego_slay);
	parser_reg(p, "brand str code", parse_ego_brand);
	parser_reg(p, "curse sym name int power", parse_ego_curse);
	return p;
}

static errr run_parse_ego(struct parser *p) {
	return parse_file_quit_not_found(p, "ego_item");
}

static errr finish_parse_ego(struct parser *p) {
	struct ego_item *e, *n;

	/* scan the list for the max id */
	z_info->e_max = 0;
	e = parser_priv(p);
	while (e) {
		if (e->eidx > z_info->e_max)
			z_info->e_max = e->eidx;
		e = e->next;
	}

	/* allocate the direct access list and copy the data to it */
	e_info = mem_zalloc((z_info->e_max + 1) * sizeof(*e));
	for (e = parser_priv(p); e; e = n) {
		memcpy(&e_info[e->eidx], e, sizeof(*e));
		n = e->next;
		if (n)
			e_info[e->eidx].next = &e_info[n->eidx];
		else
			e_info[e->eidx].next = NULL;
		mem_free(e);
	}
	z_info->e_max += 1;

	parser_destroy(p);
	return 0;
}

static void cleanup_ego(void)
{
	int idx;
	for (idx = 0; idx < z_info->e_max; idx++) {
		struct ego_item *ego = &e_info[idx];
		struct poss_item *poss;

		string_free(ego->name);
		string_free(ego->text);
		mem_free(ego->brands);
		mem_free(ego->slays);
		mem_free(ego->curses);
		free_effect(ego->effect);

		poss = ego->poss_items;
		while (poss) {
			struct poss_item *next = poss->next;
			mem_free(poss);
			poss = next;
		}
	}
	mem_free(e_info);
}

struct file_parser ego_parser = {
	"ego_item",
	init_parse_ego,
	run_parse_ego,
	finish_parse_ego,
	cleanup_ego
};

/**
 * ------------------------------------------------------------------------
 * Initialize artifacts
 * ------------------------------------------------------------------------ */

static enum parser_error parse_artifact_name(struct parser *p) {
	size_t i;
	int idx = parser_getint(p, "index");
	const char *name = parser_getstr(p, "name");
	struct artifact *h = parser_priv(p);

	struct artifact *a = mem_zalloc(sizeof *a);
	a->next = h;
	parser_setpriv(p, a);
	a->aidx = idx;
	a->name = string_make(name);

	/* Ignore all base elements */
	for (i = ELEM_BASE_MIN; i < ELEM_HIGH_MIN; i++)
		a->el_info[i].flags |= EL_INFO_IGNORE;

	return PARSE_ERROR_NONE;
}

static enum parser_error parse_artifact_base_object(struct parser *p) {
	struct artifact *a = parser_priv(p);
	int tval, sval;
	const char *sval_name;

	assert(a);

	tval = tval_find_idx(parser_getsym(p, "tval"));
	if (tval < 0)
		return PARSE_ERROR_UNRECOGNISED_TVAL;
	a->tval = tval;

	sval_name = parser_getsym(p, "sval");
	sval = lookup_sval(a->tval, sval_name);
	if (sval < 0)
		return write_dummy_object_record(a, sval_name);
	a->sval = sval;

	return PARSE_ERROR_NONE;
}

static enum parser_error parse_artifact_graphics(struct parser *p) {
	wchar_t glyph = parser_getchar(p, "glyph");
	const char *color = parser_getsym(p, "color");
	struct artifact *a = parser_priv(p);
	struct object_kind *k = lookup_kind(a->tval, a->sval);
	assert(a);
	assert(k);

	if (!kf_has(k->kind_flags, KF_INSTA_ART))
		return PARSE_ERROR_NOT_SPECIAL_ARTIFACT;

	k->d_char = glyph;
	if (strlen(color) > 1)
		k->d_attr = color_text_to_attr(color);
	else
		k->d_attr = color_char_to_attr(color[0]);

	return PARSE_ERROR_NONE;
}

static enum parser_error parse_artifact_info(struct parser *p) {
	struct artifact *a = parser_priv(p);
	assert(a);

	a->level = parser_getint(p, "level");
	a->weight = parser_getint(p, "weight");
	a->cost = parser_getint(p, "cost");
	return PARSE_ERROR_NONE;
}

static enum parser_error parse_artifact_alloc(struct parser *p) {
	struct artifact *a = parser_priv(p);
	const char *tmp = parser_getstr(p, "minmax");
	int amin, amax;
	assert(a);

	a->alloc_prob = parser_getint(p, "common");
	if (sscanf(tmp, "%d to %d", &amin, &amax) != 2)
		return PARSE_ERROR_INVALID_ALLOCATION;

	if (amin > 255 || amax > 255 || amin < 0 || amax < 0)
		return PARSE_ERROR_OUT_OF_BOUNDS;

	a->alloc_min = amin;
	a->alloc_max = amax;
	return PARSE_ERROR_NONE;
}

static enum parser_error parse_artifact_power(struct parser *p) {
	struct artifact *a = parser_priv(p);
	struct random hd = parser_getrand(p, "hd");
	assert(a);

	a->ac = parser_getint(p, "ac");
	a->dd = hd.dice;
	a->ds = hd.sides;
	a->to_h = parser_getint(p, "to-h");
	a->to_d = parser_getint(p, "to-d");
	a->to_a = parser_getint(p, "to-a");
	return PARSE_ERROR_NONE;
}

static enum parser_error parse_artifact_flags(struct parser *p) {
	struct artifact *a = parser_priv(p);
	char *s;
	char *t;
	assert(a);

	if (!parser_hasval(p, "flags"))
		return PARSE_ERROR_NONE;
	s = string_make(parser_getstr(p, "flags"));

	t = strtok(s, " |");
	while (t) {
		bool found = false;
		if (!grab_flag(a->flags, OF_SIZE, obj_flags, t))
			found = true;
		if (grab_element_flag(a->el_info, t))
			found = true;
		if (!found)
			break;
		t = strtok(NULL, " |");
	}
	mem_free(s);
	return t ? PARSE_ERROR_INVALID_FLAG : PARSE_ERROR_NONE;
}

static enum parser_error parse_artifact_act(struct parser *p) {
	struct artifact *a = parser_priv(p);
	const char *name = parser_getstr(p, "name");

	if (!a)
		return PARSE_ERROR_MISSING_RECORD_HEADER;
	a->activation = findact(name);

	return PARSE_ERROR_NONE;
}

static enum parser_error parse_artifact_time(struct parser *p) {
	struct artifact *a = parser_priv(p);
	assert(a);

	a->time = parser_getrand(p, "time");
	return PARSE_ERROR_NONE;
}

static enum parser_error parse_artifact_msg(struct parser *p) {
	struct artifact *a = parser_priv(p);
	assert(a);

	a->alt_msg = string_append(a->alt_msg, parser_getstr(p, "text"));
	return PARSE_ERROR_NONE;
}

static enum parser_error parse_artifact_values(struct parser *p) {
	struct artifact *a = parser_priv(p);
	char *s; 
	char *t;
	assert(a);

	s = string_make(parser_getstr(p, "values"));
	t = strtok(s, " |");

	while (t) {
		bool found = false;
		int value = 0;
		int index = 0;
		if (!grab_int_value(a->modifiers, obj_mods, t))
			found = true;
		if (!grab_index_and_int(&value, &index, elements, "RES_", t)) {
			found = true;
			a->el_info[index].res_level = value;
		}
		if (!found)
			break;

		t = strtok(NULL, " |");
	}

	mem_free(s);
	return t ? PARSE_ERROR_INVALID_VALUE : PARSE_ERROR_NONE;
}

static enum parser_error parse_artifact_desc(struct parser *p) {
	struct artifact *a = parser_priv(p);
	assert(a);

	a->text = string_append(a->text, parser_getstr(p, "text"));
	return PARSE_ERROR_NONE;
}

static enum parser_error parse_artifact_slay(struct parser *p) {
	struct artifact *a = parser_priv(p);
	const char *s = parser_getstr(p, "code");
	int i;

	if (!a)
		return PARSE_ERROR_MISSING_RECORD_HEADER;
	for (i = 1; i < z_info->slay_max; i++) {
		if (streq(s, slays[i].code)) break;
	}
	if (i == z_info->slay_max)
		return PARSE_ERROR_UNRECOGNISED_SLAY;

	if (!a->slays)
		a->slays = mem_zalloc(z_info->slay_max * sizeof(bool));
	a->slays[i] = true;
	return PARSE_ERROR_NONE;
}

static enum parser_error parse_artifact_brand(struct parser *p) {
	struct artifact *a = parser_priv(p);
	const char *s = parser_getstr(p, "code");
	int i;

	if (!a)
		return PARSE_ERROR_MISSING_RECORD_HEADER;
	for (i = 1; i < z_info->brand_max; i++) {
		if (streq(s, brands[i].code)) break;
	}
	if (i == z_info->brand_max)
		return PARSE_ERROR_UNRECOGNISED_BRAND;

	if (!a->brands)
		a->brands = mem_zalloc(z_info->brand_max * sizeof(bool));
	a->brands[i] = true;
	return PARSE_ERROR_NONE;
}

static enum parser_error parse_artifact_curse(struct parser *p) {
	struct artifact *a = parser_priv(p);
	char *s;
	int i;

	assert(a);
	s = string_make(parser_getsym(p, "name"));
	for (i = 1; i < z_info->curse_max; i++) {
		if (streq(s, curses[i].name)) break;
	}
	if (i == z_info->curse_max)
		return PARSE_ERROR_UNRECOGNISED_CURSE;

	if (!a->curses)
		a->curses = mem_zalloc(z_info->curse_max * sizeof(int));
	a->curses[i] = parser_getint(p, "power");
	return PARSE_ERROR_NONE;
}

struct parser *init_parse_artifact(void) {
	struct parser *p = parser_new();
	parser_setpriv(p, NULL);
	parser_reg(p, "name int index str name", parse_artifact_name);
	parser_reg(p, "base-object sym tval sym sval", parse_artifact_base_object);
	parser_reg(p, "graphics char glyph sym color", parse_artifact_graphics);
	parser_reg(p, "info int level int weight int cost", parse_artifact_info);
	parser_reg(p, "alloc int common str minmax", parse_artifact_alloc);
	parser_reg(p, "power int ac rand hd int to-h int to-d int to-a",
			   parse_artifact_power);
	parser_reg(p, "flags ?str flags", parse_artifact_flags);
	parser_reg(p, "act str name", parse_artifact_act);
	parser_reg(p, "time rand time", parse_artifact_time);
	parser_reg(p, "msg str text", parse_artifact_msg);
	parser_reg(p, "values str values", parse_artifact_values);
	parser_reg(p, "desc str text", parse_artifact_desc);
	parser_reg(p, "slay str code", parse_artifact_slay);
	parser_reg(p, "brand str code", parse_artifact_brand);
	parser_reg(p, "curse sym name int power", parse_artifact_curse);
	return p;
}

static errr run_parse_artifact(struct parser *p) {
	return parse_file_quit_not_found(p, "artifact");
}

static errr finish_parse_artifact(struct parser *p) {
	struct artifact *a, *n;
	int none;

	/* scan the list for the max id */
	z_info->a_max = 0;
	a = parser_priv(p);
	while (a) {
		if (a->aidx > z_info->a_max)
			z_info->a_max = a->aidx;
		a = a->next;
	}

	/* allocate the direct access list and copy the data to it */
	a_info = mem_zalloc((z_info->a_max + 1) * sizeof(*a));
	for (a = parser_priv(p); a; a = n) {
		memcpy(&a_info[a->aidx], a, sizeof(*a));
		n = a->next;
		if (n)
			a_info[a->aidx].next = &a_info[n->aidx];
		else
			a_info[a->aidx].next = NULL;

		mem_free(a);
	}
	z_info->a_max += 1;

	/* Now we're done with object kinds, deal with object-like things */
	none = tval_find_idx("none");
	unknown_item_kind = lookup_kind(none, lookup_sval(none, "<unknown item>"));
	unknown_gold_kind = lookup_kind(none,
									lookup_sval(none, "<unknown treasure>"));
	pile_kind = lookup_kind(none, lookup_sval(none, "<pile>"));
	curse_object_kind = lookup_kind(none, lookup_sval(none, "<curse object>"));
	write_curse_kinds();
	parser_destroy(p);
	return 0;
}

static void cleanup_artifact(void)
{
	int idx;
	for (idx = 0; idx < z_info->a_max; idx++) {
		struct artifact *art = &a_info[idx];
		string_free(art->name);
		string_free(art->alt_msg);
		string_free(art->text);
		mem_free(art->brands);
		mem_free(art->slays);
		mem_free(art->curses);
	}
	mem_free(a_info);
}

struct file_parser artifact_parser = {
	"artifact",
	init_parse_artifact,
	run_parse_artifact,
	finish_parse_artifact,
	cleanup_artifact
};
