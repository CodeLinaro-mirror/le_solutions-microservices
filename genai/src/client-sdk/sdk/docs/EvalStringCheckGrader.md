# EvalStringCheckGrader

A StringCheckGrader object that performs a string comparison between input and reference using a specified operation. 

## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**type** | **str** | The object type, which is always &#x60;string_check&#x60;. | 
**name** | **str** | The name of the grader. | 
**input** | **str** | The input text. This may include template strings. | 
**reference** | **str** | The reference text. This may include template strings. | 
**operation** | **str** | The string check operation to perform. One of &#x60;eq&#x60;, &#x60;ne&#x60;, &#x60;like&#x60;, or &#x60;ilike&#x60;. | 

## Example

```python
from openapi_client.models.eval_string_check_grader import EvalStringCheckGrader

# TODO update the JSON string below
json = "{}"
# create an instance of EvalStringCheckGrader from a JSON string
eval_string_check_grader_instance = EvalStringCheckGrader.from_json(json)
# print the JSON string representation of the object
print(EvalStringCheckGrader.to_json())

# convert the object into a dict
eval_string_check_grader_dict = eval_string_check_grader_instance.to_dict()
# create an instance of EvalStringCheckGrader from a dict
eval_string_check_grader_from_dict = EvalStringCheckGrader.from_dict(eval_string_check_grader_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


