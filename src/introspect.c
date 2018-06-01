/* PipeWire
 * Copyright (C) 2018 Wim Taymans <wim.taymans@gmail.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#include <errno.h>

#include <pipewire/log.h>

#include <pulse/introspect.h>

#include "internal.h"

typedef int (*global_filter_t)(pa_context *c, struct global *g);

static void node_event_info(void *object, struct pw_node_info *info)
{
	struct global *g = object;
	pw_log_debug("update");
	g->info = pw_node_info_update(g->info, info);
}

static const struct pw_node_proxy_events node_events = {
	PW_VERSION_NODE_PROXY_EVENTS,
	.info = node_event_info,
};

static void module_event_info(void *object, struct pw_module_info *info)
{
        struct global *g = object;
	pw_log_debug("update");
        g->info = pw_module_info_update(g->info, info);
}

static const struct pw_module_proxy_events module_events = {
	PW_VERSION_MODULE_PROXY_EVENTS,
	.info = module_event_info,
};

static void client_event_info(void *object, struct pw_client_info *info)
{
        struct global *g = object;
	pw_log_debug("update");
        g->info = pw_client_info_update(g->info, info);
}

static const struct pw_client_proxy_events client_events = {
	PW_VERSION_CLIENT_PROXY_EVENTS,
	.info = client_event_info,
};

static int ensure_global(pa_context *c, struct global *g)
{
	uint32_t client_version;
	const void *events;
	pw_destroy_t destroy;
	struct pw_type *t = c->t;

	if (g->proxy != NULL)
		return 0;

	if (g->type == t->node) {
		events = &node_events;
                client_version = PW_VERSION_NODE;
                destroy = (pw_destroy_t) pw_node_info_free;
	}
	else if (g->type == t->module) {
		events = &module_events;
                client_version = PW_VERSION_MODULE;
                destroy = (pw_destroy_t) pw_module_info_free;
	}
	else if (g->type == t->client) {
		events = &client_events;
                client_version = PW_VERSION_CLIENT;
                destroy = (pw_destroy_t) pw_client_info_free;
	}
	else
		return -EINVAL;

	g->proxy = pw_registry_proxy_bind(c->registry_proxy, g->id, g->type,
                                      client_version, 0);
	if (g->proxy == NULL)
                return -ENOMEM;

	pw_proxy_add_proxy_listener(g->proxy, &g->proxy_proxy_listener, events, g);
	g->destroy = destroy;

	return 0;
}

static void ensure_types(pa_context *c, uint32_t type, global_filter_t filter)
{
	struct global *g;
	spa_list_for_each(g, &c->globals, link) {
		if (!filter(c, g))
			continue;
		ensure_global(c, g);
	}
}

struct sink_data {
	pa_context *context;
	pa_sink_info_cb_t cb;
	void *userdata;
	struct global *global;
};

static void sink_callback(struct sink_data *d)
{
	struct global *g = d->global;
	struct pw_node_info *info = g->info;
	pa_sink_info i;

	spa_zero(i);
	i.index = g->id;
	i.name = info->name;
	i.proplist = pa_proplist_new_dict(info->props);
	i.owner_module = g->parent_id;
	i.base_volume = PA_VOLUME_NORM;
	i.n_volume_steps = PA_VOLUME_NORM+1;

	d->cb(d->context, &i, 0, d->userdata);
}

static void sink_info(pa_operation *o, void *userdata)
{
	struct sink_data *d = userdata;
	sink_callback(d);
	d->cb(d->context, NULL, 1, d->userdata);
}

static int sink_filter(pa_context *c, struct global *g)
{
	const char *str;

	if (g->type != c->t->node)
		return 0;
	if (g->props == NULL)
		return 0;
	if ((str = pw_properties_get(g->props, "media.class")) == NULL)
		return 0;
	if (strcmp(str, "Audio/Sink") != 0)
		return 0;
	return 1;
}

pa_operation* pa_context_get_sink_info_by_name(pa_context *c, const char *name, pa_sink_info_cb_t cb, void *userdata)
{
	pw_log_warn("Not Implemented");
	return NULL;
}

pa_operation* pa_context_get_sink_info_by_index(pa_context *c, uint32_t idx, pa_sink_info_cb_t cb, void *userdata)
{
	pa_operation *o;
	struct global *g;
	struct sink_data *d;

	pa_assert(c);
	pa_assert(c->refcount >= 1);
	pa_assert(cb);

	PA_CHECK_VALIDITY_RETURN_NULL(c, idx != PA_INVALID_INDEX, PA_ERR_INVALID);

	if ((g = pa_context_find_global(c, idx)) == NULL)
		return NULL;
	if (!sink_filter(c, g))
		return NULL;

	ensure_global(c, g);

	o = pa_operation_new(c, NULL, sink_info, sizeof(struct sink_data));
	d = o->userdata;
	d->global = g;
	return o;
}

static void sink_info_list(pa_operation *o, void *userdata)
{
	struct sink_data *d = userdata;
	pa_context *c = d->context;
	struct global *g;

	spa_list_for_each(g, &c->globals, link) {
		if (!sink_filter(c, g))
			continue;
		d->global = g;
		sink_callback(d);
	}
	d->cb(c, NULL, 1, d->userdata);
}

pa_operation* pa_context_get_sink_info_list(pa_context *c, pa_sink_info_cb_t cb, void *userdata)
{
	pa_operation *o;
	struct sink_data *d;

	pa_assert(c);
	pa_assert(c->refcount >= 1);
	pa_assert(cb);

	PA_CHECK_VALIDITY_RETURN_NULL(c, c->state == PA_CONTEXT_READY, PA_ERR_BADSTATE);

	ensure_types(c, c->t->node, sink_filter);
	o = pa_operation_new(c, NULL, sink_info_list, sizeof(struct sink_data));
	d = o->userdata;
	d->context = c;
	d->cb = cb;
	d->userdata = userdata;

	return o;
}

pa_operation* pa_context_set_sink_volume_by_index(pa_context *c, uint32_t idx, const pa_cvolume *volume, pa_context_success_cb_t cb, void *userdata)
{
	pw_log_warn("Not Implemented");
	return NULL;
}

pa_operation* pa_context_set_sink_volume_by_name(pa_context *c, const char *name, const pa_cvolume *volume, pa_context_success_cb_t cb, void *userdata)
{
	pw_log_warn("Not Implemented");
	return NULL;
}

pa_operation* pa_context_set_sink_mute_by_index(pa_context *c, uint32_t idx, int mute, pa_context_success_cb_t cb, void *userdata)
{
	pw_log_warn("Not Implemented");
	return NULL;
}

pa_operation* pa_context_set_sink_mute_by_name(pa_context *c, const char *name, int mute, pa_context_success_cb_t cb, void *userdata)
{
	pw_log_warn("Not Implemented");
	return NULL;
}

pa_operation* pa_context_suspend_sink_by_name(pa_context *c, const char *sink_name, int suspend, pa_context_success_cb_t cb, void* userdata)
{
	pw_log_warn("Not Implemented");
	return NULL;
}

pa_operation* pa_context_suspend_sink_by_index(pa_context *c, uint32_t idx, int suspend,  pa_context_success_cb_t cb, void* userdata)
{
	pw_log_warn("Not Implemented");
	return NULL;
}

pa_operation* pa_context_set_sink_port_by_index(pa_context *c, uint32_t idx, const char*port, pa_context_success_cb_t cb, void *userdata)
{
	pw_log_warn("Not Implemented");
	return NULL;
}

pa_operation* pa_context_set_sink_port_by_name(pa_context *c, const char*name, const char*port, pa_context_success_cb_t cb, void *userdata)
{
	pw_log_warn("Not Implemented");
	return NULL;
}


struct source_data {
	pa_context *context;
	pa_source_info_cb_t cb;
	void *userdata;
	struct global *global;
};

static void source_callback(struct source_data *d)
{
	struct global *g = d->global;
	struct pw_node_info *info = g->info;
	pa_source_info i;

	spa_zero(i);
	i.index = g->id;
	i.name = info->name;
	i.proplist = pa_proplist_new_dict(info->props);
	i.owner_module = g->parent_id;
	i.base_volume = PA_VOLUME_NORM;
	i.n_volume_steps = PA_VOLUME_NORM+1;

	d->cb(d->context, &i, 0, d->userdata);
}

static void source_info(pa_operation *o, void *userdata)
{
	struct source_data *d = userdata;
	source_callback(d);
	d->cb(d->context, NULL, 1, d->userdata);
}

static int source_filter(pa_context *c, struct global *g)
{
	const char *str;

	if (g->type != c->t->node)
		return 0;
	if (g->props == NULL)
		return 0;
	if ((str = pw_properties_get(g->props, "media.class")) == NULL)
		return 0;
	if (strcmp(str, "Audio/Source") != 0)
		return 0;
	return 1;
}

pa_operation* pa_context_get_source_info_by_name(pa_context *c, const char *name, pa_source_info_cb_t cb, void *userdata)
{
	pw_log_warn("Not Implemented");
	return NULL;
}

pa_operation* pa_context_get_source_info_by_index(pa_context *c, uint32_t idx, pa_source_info_cb_t cb, void *userdata)
{
	pa_operation *o;
	struct global *g;
	struct source_data *d;

	pa_assert(c);
	pa_assert(c->refcount >= 1);
	pa_assert(cb);

	PA_CHECK_VALIDITY_RETURN_NULL(c, idx != PA_INVALID_INDEX, PA_ERR_INVALID);

	if ((g = pa_context_find_global(c, idx)) == NULL)
		return NULL;
	if (!source_filter(c, g))
		return NULL;

	ensure_global(c, g);

	o = pa_operation_new(c, NULL, source_info, sizeof(struct source_data));
	d = o->userdata;
	d->global = g;
	return o;
}

static void source_info_list(pa_operation *o, void *userdata)
{
	struct source_data *d = userdata;
	pa_context *c = d->context;
	struct global *g;

	spa_list_for_each(g, &c->globals, link) {
		if (!source_filter(c, g))
			continue;
		d->global = g;
		source_callback(d);
	}
	d->cb(c, NULL, 1, d->userdata);
}

pa_operation* pa_context_get_source_info_list(pa_context *c, pa_source_info_cb_t cb, void *userdata)
{
	pa_operation *o;
	struct source_data *d;

	pa_assert(c);
	pa_assert(c->refcount >= 1);
	pa_assert(cb);

	PA_CHECK_VALIDITY_RETURN_NULL(c, c->state == PA_CONTEXT_READY, PA_ERR_BADSTATE);

	ensure_types(c, c->t->node, source_filter);
	o = pa_operation_new(c, NULL, source_info_list, sizeof(struct source_data));
	d = o->userdata;
	d->context = c;
	d->cb = cb;
	d->userdata = userdata;

	return o;
}

pa_operation* pa_context_set_source_volume_by_index(pa_context *c, uint32_t idx, const pa_cvolume *volume, pa_context_success_cb_t cb, void *userdata)
{
	pw_log_warn("Not Implemented");
	return NULL;
}

pa_operation* pa_context_set_source_volume_by_name(pa_context *c, const char *name, const pa_cvolume *volume, pa_context_success_cb_t cb, void *userdata)
{
	pw_log_warn("Not Implemented");
	return NULL;
}

pa_operation* pa_context_set_source_mute_by_index(pa_context *c, uint32_t idx, int mute, pa_context_success_cb_t cb, void *userdata)
{
	pw_log_warn("Not Implemented");
	return NULL;
}

pa_operation* pa_context_set_source_mute_by_name(pa_context *c, const char *name, int mute, pa_context_success_cb_t cb, void *userdata)
{
	pw_log_warn("Not Implemented");
	return NULL;
}

pa_operation* pa_context_suspend_source_by_name(pa_context *c, const char *source_name, int suspend, pa_context_success_cb_t cb, void* userdata)
{
	pw_log_warn("Not Implemented");
	return NULL;
}

pa_operation* pa_context_suspend_source_by_index(pa_context *c, uint32_t idx, int suspend, pa_context_success_cb_t cb, void* userdata)
{
	pw_log_warn("Not Implemented");
	return NULL;
}

pa_operation* pa_context_set_source_port_by_index(pa_context *c, uint32_t idx, const char*port, pa_context_success_cb_t cb, void *userdata)
{
	pw_log_warn("Not Implemented");
	return NULL;
}

pa_operation* pa_context_set_source_port_by_name(pa_context *c, const char*name, const char*port, pa_context_success_cb_t cb, void *userdata)
{
	pw_log_warn("Not Implemented");
	return NULL;
}

pa_operation* pa_context_get_server_info(pa_context *c, pa_server_info_cb_t cb, void *userdata)
{
	pw_log_warn("Not Implemented");
	return NULL;
}

struct module_data {
	pa_context *context;
	pa_module_info_cb_t cb;
	void *userdata;
	struct global *global;
};

static void module_callback(struct module_data *d)
{
	struct global *g = d->global;
	struct pw_module_info *info = g->info;
	pa_module_info i;

	spa_zero(i);
	i.proplist = pa_proplist_new_dict(info->props);
	i.index = g->id;
	i.name = info->name;
	i.argument = info->args;
	i.n_used = -1;
	i.auto_unload = false;
	d->cb(d->context, &i, 0, d->userdata);
}

static void module_info(pa_operation *o, void *userdata)
{
	struct module_data *d = userdata;
	module_callback(d);
	d->cb(d->context, NULL, 1, d->userdata);
}

static int module_filter(pa_context *c, struct global *g)
{
	if (g->type != c->t->module)
		return 0;
	return 1;
}

pa_operation* pa_context_get_module_info(pa_context *c, uint32_t idx, pa_module_info_cb_t cb, void *userdata)
{
	pa_operation *o;
	struct global *g;
	struct module_data *d;

	pa_assert(c);
	pa_assert(c->refcount >= 1);
	pa_assert(cb);

	PA_CHECK_VALIDITY_RETURN_NULL(c, idx != PA_INVALID_INDEX, PA_ERR_INVALID);

	if ((g = pa_context_find_global(c, idx)) == NULL)
		return NULL;
	if (!module_filter(c, g))
		return NULL;

	ensure_global(c, g);

	o = pa_operation_new(c, NULL, module_info, sizeof(struct module_data));
	d = o->userdata;
	d->global = g;

	return o;
}

static void module_info_list(pa_operation *o, void *userdata)
{
	struct module_data *d = userdata;
	pa_context *c = d->context;
	struct global *g;

	spa_list_for_each(g, &c->globals, link) {
		if (!module_filter(c, g))
			continue;
		d->global = g;
		module_callback(d);
	}
	d->cb(c, NULL, 1, d->userdata);
}

pa_operation* pa_context_get_module_info_list(pa_context *c, pa_module_info_cb_t cb, void *userdata)
{
	pa_operation *o;
	struct module_data *d;

	pa_assert(c);
	pa_assert(c->refcount >= 1);
	pa_assert(cb);

	PA_CHECK_VALIDITY_RETURN_NULL(c, c->state == PA_CONTEXT_READY, PA_ERR_BADSTATE);

	ensure_types(c, c->t->module, module_filter);
	o = pa_operation_new(c, NULL, module_info_list, sizeof(struct module_data));
	d = o->userdata;
	d->context = c;
	d->cb = cb;
	d->userdata = userdata;

	return o;
}

pa_operation* pa_context_load_module(pa_context *c, const char*name, const char *argument, pa_context_index_cb_t cb, void *userdata)
{
	pw_log_warn("Not Implemented");
	return NULL;
}

pa_operation* pa_context_unload_module(pa_context *c, uint32_t idx, pa_context_success_cb_t cb, void *userdata)
{
	pw_log_warn("Not Implemented");
	return NULL;
}

struct client_data {
	pa_context *context;
	pa_client_info_cb_t cb;
	void *userdata;
	struct global *global;
};

static void client_callback(struct client_data *d)
{
	struct global *g = d->global;
	struct pw_client_info *info = g->info;
	pa_client_info i;

	spa_zero(i);
	i.proplist = pa_proplist_new_dict(info->props);
	i.index = g->id;
	i.name = info->props ?
		spa_dict_lookup(info->props, "application.prgname") : NULL;
	i.owner_module = g->parent_id;
	i.driver = info->props ?
		spa_dict_lookup(info->props, PW_CLIENT_PROP_PROTOCOL) : NULL;
	d->cb(d->context, &i, 0, d->userdata);
}

static void client_info(pa_operation *o, void *userdata)
{
	struct client_data *d = userdata;
	client_callback(d);
	d->cb(d->context, NULL, 1, d->userdata);
}

static int client_filter(pa_context *c, struct global *g)
{
	if (g->type != c->t->client)
		return 0;
	return 1;
}

pa_operation* pa_context_get_client_info(pa_context *c, uint32_t idx, pa_client_info_cb_t cb, void *userdata)
{
	pa_operation *o;
	struct global *g;
	struct client_data *d;

	pa_assert(c);
	pa_assert(c->refcount >= 1);
	pa_assert(cb);

	PA_CHECK_VALIDITY_RETURN_NULL(c, idx != PA_INVALID_INDEX, PA_ERR_INVALID);

	if ((g = pa_context_find_global(c, idx)) == NULL)
		return NULL;
	if (!client_filter(c, g))
		return NULL;

	ensure_global(c, g);

	o = pa_operation_new(c, NULL, client_info, sizeof(struct client_data));
	d = o->userdata;
	d->global = g;

	return o;
}

static void client_info_list(pa_operation *o, void *userdata)
{
	struct client_data *d = userdata;
	pa_context *c = d->context;
	struct global *g;

	spa_list_for_each(g, &c->globals, link) {
		if (!client_filter(c, g))
			continue;
		d->global = g;
		client_callback(d);
	}
	d->cb(c, NULL, 1, d->userdata);
}

pa_operation* pa_context_get_client_info_list(pa_context *c, pa_client_info_cb_t cb, void *userdata)
{
	pa_operation *o;
	struct client_data *d;

	pa_assert(c);
	pa_assert(c->refcount >= 1);
	pa_assert(cb);

	PA_CHECK_VALIDITY_RETURN_NULL(c, c->state == PA_CONTEXT_READY, PA_ERR_BADSTATE);

	ensure_types(c, c->t->client, client_filter);
	o = pa_operation_new(c, NULL, client_info_list, sizeof(struct client_data));
	d = o->userdata;
	d->context = c;
	d->cb = cb;
	d->userdata = userdata;

	return o;
}

pa_operation* pa_context_kill_client(pa_context *c, uint32_t idx, pa_context_success_cb_t cb, void *userdata)
{
	pw_log_warn("Not Implemented");
	return NULL;
}

pa_operation* pa_context_get_card_info_by_index(pa_context *c, uint32_t idx, pa_card_info_cb_t cb, void *userdata)
{
	pw_log_warn("Not Implemented");
	return NULL;
}

pa_operation* pa_context_get_card_info_by_name(pa_context *c, const char *name, pa_card_info_cb_t cb, void *userdata)
{
	pw_log_warn("Not Implemented");
	return NULL;
}

pa_operation* pa_context_get_card_info_list(pa_context *c, pa_card_info_cb_t cb, void *userdata)
{
	pw_log_warn("Not Implemented");
	return NULL;
}

pa_operation* pa_context_set_card_profile_by_index(pa_context *c, uint32_t idx, const char*profile, pa_context_success_cb_t cb, void *userdata)
{
	pw_log_warn("Not Implemented");
	return NULL;
}

pa_operation* pa_context_set_card_profile_by_name(pa_context *c, const char*name, const char*profile, pa_context_success_cb_t cb, void *userdata)
{
	pw_log_warn("Not Implemented");
	return NULL;
}

pa_operation* pa_context_set_port_latency_offset(pa_context *c, const char *card_name, const char *port_name, int64_t offset, pa_context_success_cb_t cb, void *userdata)
{
	pw_log_warn("Not Implemented");
	return NULL;
}

pa_operation* pa_context_get_sink_input_info(pa_context *c, uint32_t idx, pa_sink_input_info_cb_t cb, void *userdata)
{
	pw_log_warn("Not Implemented");
	return NULL;
}

pa_operation* pa_context_get_sink_input_info_list(pa_context *c, pa_sink_input_info_cb_t cb, void *userdata)
{
	pw_log_warn("Not Implemented");
	return NULL;
}

pa_operation* pa_context_move_sink_input_by_name(pa_context *c, uint32_t idx, const char *sink_name, pa_context_success_cb_t cb, void* userdata)
{
	pw_log_warn("Not Implemented");
	return NULL;
}

pa_operation* pa_context_move_sink_input_by_index(pa_context *c, uint32_t idx, uint32_t sink_idx, pa_context_success_cb_t cb, void* userdata)
{
	pw_log_warn("Not Implemented");
	return NULL;
}

pa_operation* pa_context_set_sink_input_volume(pa_context *c, uint32_t idx, const pa_cvolume *volume, pa_context_success_cb_t cb, void *userdata)
{
	pw_log_warn("Not Implemented");
	return NULL;
}

pa_operation* pa_context_set_sink_input_mute(pa_context *c, uint32_t idx, int mute, pa_context_success_cb_t cb, void *userdata)
{
	pw_log_warn("Not Implemented");
	return NULL;
}

pa_operation* pa_context_kill_sink_input(pa_context *c, uint32_t idx, pa_context_success_cb_t cb, void *userdata)
{
	pw_log_warn("Not Implemented");
	return NULL;
}

pa_operation* pa_context_get_source_output_info(pa_context *c, uint32_t idx, pa_source_output_info_cb_t cb, void *userdata)
{
	pw_log_warn("Not Implemented");
	return NULL;
}

pa_operation* pa_context_get_source_output_info_list(pa_context *c, pa_source_output_info_cb_t cb, void *userdata)
{
	pw_log_warn("Not Implemented");
	return NULL;
}

pa_operation* pa_context_move_source_output_by_name(pa_context *c, uint32_t idx, const char *source_name, pa_context_success_cb_t cb, void* userdata)
{
	pw_log_warn("Not Implemented");
	return NULL;
}

pa_operation* pa_context_move_source_output_by_index(pa_context *c, uint32_t idx, uint32_t source_idx, pa_context_success_cb_t cb, void* userdata)
{
	pw_log_warn("Not Implemented");
	return NULL;
}

pa_operation* pa_context_set_source_output_volume(pa_context *c, uint32_t idx, const pa_cvolume *volume, pa_context_success_cb_t cb, void *userdata)
{
	pw_log_warn("Not Implemented");
	return NULL;
}

pa_operation* pa_context_set_source_output_mute(pa_context *c, uint32_t idx, int mute, pa_context_success_cb_t cb, void *userdata)
{
	pw_log_warn("Not Implemented");
	return NULL;
}

pa_operation* pa_context_kill_source_output(pa_context *c, uint32_t idx, pa_context_success_cb_t cb, void *userdata)
{
	pw_log_warn("Not Implemented");
	return NULL;
}

pa_operation* pa_context_stat(pa_context *c, pa_stat_info_cb_t cb, void *userdata)
{
	pw_log_warn("Not Implemented");
	return NULL;
}

pa_operation* pa_context_get_sample_info_by_name(pa_context *c, const char *name, pa_sample_info_cb_t cb, void *userdata)
{
	pw_log_warn("Not Implemented");
	return NULL;
}

pa_operation* pa_context_get_sample_info_by_index(pa_context *c, uint32_t idx, pa_sample_info_cb_t cb, void *userdata)
{
	pw_log_warn("Not Implemented");
	return NULL;
}

pa_operation* pa_context_get_sample_info_list(pa_context *c, pa_sample_info_cb_t cb, void *userdata)
{
	pw_log_warn("Not Implemented");
	return NULL;
}

pa_operation* pa_context_get_autoload_info_by_name(pa_context *c, const char *name, pa_autoload_type_t type, pa_autoload_info_cb_t cb, void *userdata)
{
	pw_log_warn("Deprecated: Not Implemented");
	return NULL;
}

pa_operation* pa_context_get_autoload_info_by_index(pa_context *c, uint32_t idx, pa_autoload_info_cb_t cb, void *userdata)
{
	pw_log_warn("Deprecated: Not Implemented");
	return NULL;
}

pa_operation* pa_context_get_autoload_info_list(pa_context *c, pa_autoload_info_cb_t cb, void *userdata)
{
	pw_log_warn("Deprecated: Not Implemented");
	return NULL;
}

pa_operation* pa_context_add_autoload(pa_context *c, const char *name, pa_autoload_type_t type, const char *module, const char*argument, pa_context_index_cb_t cb, void* userdata)
{
	pw_log_warn("Deprecated: Not Implemented");
	return NULL;
}

pa_operation* pa_context_remove_autoload_by_name(pa_context *c, const char *name, pa_autoload_type_t type, pa_context_success_cb_t cb, void* userdata)
{
	pw_log_warn("Deprecated: Not Implemented");
	return NULL;
}

pa_operation* pa_context_remove_autoload_by_index(pa_context *c, uint32_t idx, pa_context_success_cb_t cb, void* userdata)
{
	pw_log_warn("Deprecated: Not Implemented");
	return NULL;
}
