/*
 * kobject.h - generic kernel object infrastructure.
 *
 */

#if defined(__KERNEL__) && !defined(_KOBJECT_H_)
#define _KOBJECT_H_

#include <linux/types.h>
#include <linux/list.h>
#include <linux/sysfs.h>
#include <linux/rwsem.h>
#include <asm/atomic.h>

#define KOBJ_NAME_LEN	16

struct kobject {
	char			name[KOBJ_NAME_LEN];
	atomic_t		refcount;
	struct list_head	entry;
	struct kobject		* parent;
	struct kset		* kset;
	struct kobj_type	* ktype;
	struct dentry		* dentry;
};

extern void kobject_init(struct kobject *);
extern void kobject_cleanup(struct kobject *);

extern int kobject_add(struct kobject *);
extern void kobject_del(struct kobject *);

extern int kobject_register(struct kobject *);
extern void kobject_unregister(struct kobject *);

extern struct kobject * kobject_get(struct kobject *);
extern void kobject_put(struct kobject *);


struct kobj_type {
	void (*release)(struct kobject *);
	struct sysfs_ops	* sysfs_ops;
	struct attribute	** default_attrs;
};


/**
 *	kset - a set of kobjects of a specific type, belonging
 *	to a specific subsystem.
 *
 *	All kobjects of a kset should be embedded in an identical 
 *	type. This type may have a descriptor, which the kset points
 *	to. This allows there to exist sets of objects of the same
 *	type in different subsystems.
 *
 *	A subsystem does not have to be a list of only one type 
 *	of object; multiple ksets can belong to one subsystem. All 
 *	ksets of a subsystem share the subsystem's lock.
 *
 */
struct kset {
	struct subsystem	* subsys;
	struct kobj_type	* ktype;
	struct list_head	list;
	struct kobject		kobj;
};


extern void kset_init(struct kset * k);
extern int kset_add(struct kset * k);
extern int kset_register(struct kset * k);
extern void kset_unregister(struct kset * k);

static inline struct kset * to_kset(struct kobject * kobj)
{
	return container_of(kobj,struct kset,kobj);
}

static inline struct kset * kset_get(struct kset * k)
{
	return k ? to_kset(kobject_get(&k->kobj)) : NULL;
}

static inline void kset_put(struct kset * k)
{
	kobject_put(&k->kobj);
}


extern struct kobject * kset_find_obj(struct kset *, char *);


struct subsystem {
	struct kset		kset;
	struct rw_semaphore	rwsem;
};

#define decl_subsys(_name,_type) \
struct subsystem _name##_subsys = { \
	.kset = { \
		.kobj = { .name = __stringify(_name) }, \
		.ktype = _type, \
	} \
}


/**
 * Helpers for setting the kset of registered objects.
 * Often, a registered object belongs to a kset embedded in a 
 * subsystem. These do no magic, just make the resulting code
 * easier to follow. 
 */

/**
 *	kobj_set_kset_s(obj,subsys) - set kset for embedded kobject.
 *	@obj:		ptr to some object type.
 *	@subsys:	a subsystem object (not a ptr).
 *
 *	Can be used for any object type with an embedded ->kobj.
 */

#define kobj_set_kset_s(obj,subsys) \
	(obj)->kobj.kset = &(subsys).kset

/**
 *	kset_set_kset_s(obj,subsys) - set kset for embedded kset.
 *	@obj:		ptr to some object type.
 *	@subsys:	a subsystem object (not a ptr).
 *
 *	Can be used for any object type with an embedded ->kset.
 *	Sets the kset of @obj's  embedded kobject (via its embedded
 *	kset) to @subsys.kset. This makes @obj a member of that 
 *	kset.
 */

#define kset_set_kset_s(obj,subsys) \
	(obj)->kset.kobj.kset = &(subsys).kset

/**
 *	subsys_set_kset(obj,subsys) - set kset for subsystem
 *	@obj:		ptr to some object type.
 *	@subsys:	a subsystem object (not a ptr).
 *
 *	Can be used for any object type with an embedded ->subsys.
 *	Sets the kset of @obj's kobject to @subsys.kset. This makes
 *	the object a member of that kset.
 */

#define subsys_set_kset(obj,_subsys) \
	(obj)->subsys.kset.kobj.kset = &(_subsys).kset

extern void subsystem_init(struct subsystem *);
extern int subsystem_register(struct subsystem *);
extern void subsystem_unregister(struct subsystem *);

static inline struct subsystem * subsys_get(struct subsystem * s)
{
	return s ? container_of(kset_get(&s->kset),struct subsystem,kset) : NULL;
}

static inline void subsys_put(struct subsystem * s)
{
	kset_put(&s->kset);
}

struct subsys_attribute {
	struct attribute attr;
	ssize_t (*show)(struct subsystem *, char *);
	ssize_t (*store)(struct subsystem *, const char *, size_t); 
};

extern int subsys_create_file(struct subsystem * , struct subsys_attribute *);
extern void subsys_remove_file(struct subsystem * , struct subsys_attribute *);


#endif /* _KOBJECT_H_ */
