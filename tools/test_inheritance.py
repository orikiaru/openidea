import unittest
from check_inheritance import violations


class InheritanceTests(unittest.TestCase):
    def test_multiple(self):
        self.assertEqual(violations('class A : public B, public C {};'), [1])
        self.assertEqual(violations('struct A\n: B<int, float>, C {};'), [1])

    def test_single(self):
        self.assertEqual(violations('struct [[nodiscard]] A final : B<int, float> {};'), [])
        self.assertEqual(violations('struct A : B<C<int, int>> {};'), [])

    def test_comments_strings(self):
        self.assertEqual(violations('// class A : B, C {};\nconst char* s="struct A:B,C{};";'), [])
        self.assertEqual(violations('auto s=R"xx(class A:B,C{};)xx";'), [])

    def test_template_parameters(self):
        self.assertEqual(violations('template<class T, class U> struct X : T {};'), [])
        self.assertEqual(violations('enum class X : int {A, B};'), [])


if __name__ == '__main__':
    unittest.main()
