// Copyright 2010-2012 RethinkDB, all rights reserved.
#include "rdb_protocol/env.hpp"
#include "rdb_protocol/meta_utils.hpp"
#include "rdb_protocol/pb_utils.hpp"
#include "rdb_protocol/term.hpp"
#include "rdb_protocol/val.hpp"

namespace ql {

table_t::table_t(env_t *_env, uuid_u db_id, const std::string &name, bool _use_outdated)
    : env(_env), use_outdated(_use_outdated) {
    name_string_t table_name;
    bool b = table_name.assign_value(name);
    rcheck(b, strprintf("table name %s invalid (%s)", name.c_str(), valid_char_msg));
    cow_ptr_t<namespaces_semilattice_metadata_t<rdb_protocol_t> >
        namespaces_metadata = env->namespaces_semilattice_metadata->get();
    cow_ptr_t<namespaces_semilattice_metadata_t<rdb_protocol_t> >::change_t
        namespaces_metadata_change(&namespaces_metadata);
    metadata_searcher_t<namespace_semilattice_metadata_t<rdb_protocol_t> >
        ns_searcher(&namespaces_metadata_change.get()->namespaces);
    //TODO: fold into iteration below
    namespace_predicate_t pred(&table_name, &db_id);
    uuid_u id = meta_get_uuid(&ns_searcher, pred, "FIND_TABLE " + table_name.str());

    access.init(new namespace_repo_t<rdb_protocol_t>::access_t(
                    env->ns_repo, id, env->interruptor));

    metadata_search_status_t status;
    metadata_searcher_t<namespace_semilattice_metadata_t<rdb_protocol_t> >::iterator
        ns_metadata_it = ns_searcher.find_uniq(pred, &status);
    meta_check(status, METADATA_SUCCESS, "FIND_TABLE " + table_name.str());
    guarantee(!ns_metadata_it->second.is_deleted());
    r_sanity_check(!ns_metadata_it->second.get().primary_key.in_conflict());
    pkey =  ns_metadata_it->second.get().primary_key.get();
}

datum_t *table_t::env_add_ptr(datum_t *d) {
    return env->add_ptr(d);
}

const datum_t *table_t::_replace(const datum_t *orig, const map_wire_func_t &mwf,
                                UNUSED bool _so_the_template_matches) {
    const std::string &pk = get_pkey();
    if (orig->get_type() == datum_t::R_NULL) {
        map_wire_func_t mwf2 = mwf;
        orig = mwf2.compile(env)->call(orig)->as_datum();
        if (orig->get_type() == datum_t::R_NULL) {
            datum_t *resp = env->add_ptr(new datum_t(datum_t::R_OBJECT));
            const datum_t *num_1 = env->add_ptr(new ql::datum_t(1));
            bool b = resp->add("skipped", num_1);
            r_sanity_check(!b);
            return resp;
        }
    }
    store_key_t store_key(orig->el(pk)->print_primary());
    rdb_protocol_t::write_t write(rdb_protocol_t::point_replace_t(pk, store_key, mwf));

    rdb_protocol_t::write_response_t response;
    access->get_namespace_if()->write(
        write, &response, order_token_t::ignore, env->interruptor);
    Datum *d = boost::get<Datum>(&response.response);
    return env->add_ptr(new datum_t(d, env));
}


const datum_t *table_t::_replace(const datum_t *orig, func_t *f, bool nondet_ok) {
    if (f->is_deterministic()) {
        return _replace(orig, map_wire_func_t(env, f));
    } else {
        r_sanity_check(nondet_ok);
        return _replace(orig, f->call(orig)->as_datum(), true);
    }
}

const datum_t *table_t::_replace(const datum_t *orig, const datum_t *d, bool upsert) {
    Term2 t;
    int x = env->gensym();
    Term2 *arg = pb::set_func(&t, x);
    if (upsert) {
        d->write_to_protobuf(pb::set_datum(arg));
    } else {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wshadow"
        N3(BRANCH,
           N2(EQ, pb::set_var(arg, x), pb::set_null(arg)),
           d->write_to_protobuf(pb::set_datum(arg)),
           N1(ERROR, pb::set_str(arg, "Duplicate primary key.")))
#pragma GCC diagnostic pop
    }

    return _replace(orig, map_wire_func_t(t, 0, 1));
}

const std::string &table_t::get_pkey() { return pkey; }

const datum_t *table_t::get_row(const datum_t *pval) {
    std::string pks = pval->print_primary();
    rdb_protocol_t::read_t read((rdb_protocol_t::point_read_t(store_key_t(pks))));
    rdb_protocol_t::read_response_t res;
    if (use_outdated) {
        access->get_namespace_if()->read_outdated(read, &res, env->interruptor);
    } else {
        access->get_namespace_if()->read(
            read, &res, order_token_t::ignore, env->interruptor);
    }
    rdb_protocol_t::point_read_response_t *p_res =
        boost::get<rdb_protocol_t::point_read_response_t>(&res.response);
    r_sanity_check(p_res);
    return env->add_ptr(new datum_t(p_res->data, env));
}

datum_stream_t *table_t::as_datum_stream(backtrace_t::frame_t f) {
    return env->add_ptr(new lazy_datum_stream_t(env, use_outdated, access.get(), f));
}

val_t::type_t::type_t(val_t::type_t::raw_type_t _raw_type) : raw_type(_raw_type) { }

bool raw_type_is_convertible(val_t::type_t::raw_type_t _t1,
                             val_t::type_t::raw_type_t _t2) {
    const int t1 = _t1, t2 = _t2,
        DB = val_t::type_t::DB,
        TABLE = val_t::type_t::TABLE,
        SELECTION = val_t::type_t::SELECTION,
        SEQUENCE = val_t::type_t::SEQUENCE,
        SINGLE_SELECTION = val_t::type_t::SINGLE_SELECTION,
        DATUM = val_t::type_t::DATUM,
        FUNC = val_t::type_t::FUNC;
    switch(t1) {
    case DB: return t2 == DB;
    case TABLE: return t2 == TABLE || t2 == SELECTION || t2 == SEQUENCE;
    case SELECTION: return t2 == SELECTION || t2 == SEQUENCE;
    case SEQUENCE: return t2 == SEQUENCE;
    case SINGLE_SELECTION: return t2 == SINGLE_SELECTION || t2 == DATUM;
    case DATUM: return t2 == DATUM || t2 == SEQUENCE;
    case FUNC: return t2 == FUNC;
    default: unreachable();
    }
    unreachable();
}
bool val_t::type_t::is_convertible(type_t rhs) const {
    return raw_type_is_convertible(raw_type, rhs.raw_type);
}

const char *val_t::type_t::name() const {
    switch(raw_type) {
    case DB: return "DATABASE";
    case TABLE: return "TABLE";
    case SELECTION: return "SELECTION";
    case SEQUENCE: return "SEQUENCE";
    case SINGLE_SELECTION: return "SINGLE_SELECTION";
    case DATUM: return "DATUM";
    case FUNC: return "FUNCTION";
    default: unreachable();
    }
    unreachable();
}

val_t::val_t(const datum_t *_datum, const term_t *_parent, env_t *_env)
    : parent(_parent), env(_env),
      type(type_t::DATUM),
      table(0),
      sequence(0),
      datum(env->add_ptr(_datum)),
      func(0) {
    guarantee(datum);
}
val_t::val_t(const datum_t *_datum, table_t *_table, const term_t *_parent, env_t *_env)
    : parent(_parent), env(_env),
      type(type_t::SINGLE_SELECTION),
      table(env->add_ptr(_table)),
      sequence(0),
      datum(env->add_ptr(_datum)),
      func(0) {
    guarantee(table); guarantee(datum);
}
val_t::val_t(datum_stream_t *_sequence, const term_t *_parent, env_t *_env)
    : parent(_parent), env(_env),
      type(type_t::SEQUENCE),
      table(0),
      sequence(env->add_ptr(_sequence)),
      datum(0),
      func(0) {
    guarantee(sequence);
    // Some streams are really arrays in disguise.
    if ((datum = sequence->as_arr())) {
        sequence = 0;
        type = type_t::DATUM;
    }
}

val_t::val_t(table_t *_table, datum_stream_t *_sequence,
             const term_t *_parent, env_t *_env)
    : parent(_parent), env(_env),
      type(type_t::SELECTION),
      table(env->add_ptr(_table)),
      sequence(env->add_ptr(_sequence)),
      datum(0),
      func(0) {
    guarantee(table);
    guarantee(sequence);
}

val_t::val_t(table_t *_table, const term_t *_parent, env_t *_env)
    : parent(_parent), env(_env),
      type(type_t::TABLE),
      table(env->add_ptr(_table)),
      sequence(0),
      datum(0),
      func(0) {
    guarantee(table);
}
val_t::val_t(uuid_u _db, const term_t *_parent, env_t *_env)
    : parent(_parent), env(_env),
      type(type_t::DB),
      db(_db),
      table(0),
      sequence(0),
      datum(0),
      func(0) {
}
val_t::val_t(func_t *_func, const term_t *_parent, env_t *_env)
    : parent(_parent), env(_env),
      type(type_t::FUNC),
      table(0),
      sequence(0),
      datum(0),
      func(env->add_ptr(_func)) {
    guarantee(func);
}

val_t::type_t val_t::get_type() const { return type; }

const datum_t *val_t::as_datum() {
    try {
        if (type.raw_type != type_t::DATUM
            && type.raw_type != type_t::SINGLE_SELECTION) {
            rfail("Type error: cannot convert %s to DATUM.", type.name());
        }
        return datum;
    } CATCH_WITH_BT(parent->get_bt());
}

table_t *val_t::as_table() {
    try {
        rcheck(type.raw_type == type_t::TABLE,
               strprintf("Type error: cannot convert %s to TABLE.", type.name()));
        return table;
    } CATCH_WITH_BT(parent->get_bt());
}

datum_stream_t *val_t::as_seq() {
    try {
        if (type.raw_type == type_t::SEQUENCE || type.raw_type == type_t::SELECTION) {
            // passthru
        } else if (type.raw_type == type_t::TABLE) {
            if (!sequence) sequence = table->as_datum_stream(parent->get_bt());
        } else if (type.raw_type == type_t::DATUM) {
            if (!sequence) sequence = datum->as_datum_stream(env, parent->get_bt());
        } else {
            rfail("Type error: cannot convert %s to SEQUENCE.", type.name());
        }
        return sequence;
    } CATCH_WITH_BT(parent->get_bt());
}

std::pair<table_t *, datum_stream_t *> val_t::as_selection() {
    try {
        rcheck(type.raw_type == type_t::TABLE || type.raw_type == type_t::SELECTION,
               strprintf("Type error: cannot convert %s to SELECTION.", type.name()));
        return std::make_pair(table, as_seq());
    } CATCH_WITH_BT(parent->get_bt());
}

std::pair<table_t *, const datum_t *> val_t::as_single_selection() {
    try {
        rcheck(type.raw_type == type_t::SINGLE_SELECTION,
               strprintf("Type error: cannot convert %s to SINGLE_SELECTION.",
                         type.name()));
        return std::make_pair(table, datum);
    } CATCH_WITH_BT(parent->get_bt());
}

func_t *val_t::as_func(shortcut_ok_bool_t shortcut_ok) {
    try {
        if (get_type().is_convertible(type_t::DATUM) && shortcut_ok == SHORTCUT_OK) {
            if (!func) {
                r_sanity_check(parent);
                func = env->add_ptr(func_t::new_shortcut_func(env, as_datum(),
                                                              parent->get_bt()));
            }
            return func;
        }
        rcheck(type.raw_type == type_t::FUNC,
               strprintf("Type error: cannot convert %s to FUNC.", type.name()));
        return func;
    } CATCH_WITH_BT(parent->get_bt());
}

uuid_u val_t::as_db() {
    try {
        rcheck(type.raw_type == type_t::DB,
               strprintf("Type error: cannot convert %s to DB.", type.name()));
        return db;
    } CATCH_WITH_BT(parent->get_bt());
}

bool as_bool() {
    try {
        return as_datum()->as_bool();
    } CATCH_WITH_BT(parent->get_bt());
}
double as_num() {
    try {
        return as_datum()->as_num();
    } CATCH_WITH_BT(parent->get_bt());
}
int as_int() {
    try {
        return as_datum()->as_int();
    } CATCH_WITH_BT(parent->get_bt());
}
const std::string &as_str() {
    try {
        return as_datum()->as_str();
    } CATCH_WITH_BT(parent->get_bt());
}

} //namespace ql
