#ifndef TREE_H
#define TREE_H

extern int tree_init(void);
extern struct inode *get_tree_node(unsigned long);
extern struct inode *get_tree_child(struct inode *, const char *);
extern struct inode *get_tree_sibling(struct inode *, off_t);
extern struct inode *add_tree_node(struct inode *, const char *,
                                   unsigned, mode_t);
extern size_t count_tree_children(struct inode *);
extern void tree_path(struct inode *, char *, size_t, struct inode **);
#endif
